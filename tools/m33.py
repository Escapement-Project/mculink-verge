"""When each instruction issues on this part's Cortex-M33, and when each bus access happens.

cycles.py used to charge a fixed cost per instruction class. That cannot see the two things that
decide where an edge lands on this part: instruction fetch from SRAM shares S-AHB with every GPIO
access, and the core issues some 16-bit pairs together. This engine models both, from first
principles where the core's documentation allows and from vendor 0x88 measurements where it does
not (tools/lab.py records them).

It is online: the emulator calls `issue()` from its code hook, before the instruction runs, and
gets the cycle the instruction issues on. A load or store then reaches the bus at a cycle known
before the access executes, which is what lets a modelled target answer by time rather than by
order. Everything it needs about the future is the code itself, read from memory, so it is
causal: no instruction's timing depends on one that issues after it.
"""

from dataclasses import dataclass, field


# Buses, by the address being fetched or accessed.
C_AHB, S_AHB, PPB = "c", "s", "p"


def bus_of(address: int) -> str:
    if address < 0x20000000:
        return C_AHB
    if address >= 0xE0000000:
        return PPB
    return S_AHB


@dataclass
class Params:
    # From a fetch's bus slot to the cycle its word can issue an instruction, decode included.
    fetch_to_issue: int = 2
    # The same from SRAM over S-AHB, which a branch shows and straight-line code hides.
    fetch_to_issue_sram: int = 2
    # Words the prefetch buffer holds, and how many cycles after an instruction issues the words
    # it came from are free for new fetches.
    queue_words: int = 3
    queue_release: int = 0
    # Cycles after a branch issues that the fetch of its target takes the bus. Negative means
    # the core redirects before the branch itself issues; it never redirects before it has the
    # branch's own bytes.
    redirect_uncond: int = 0
    redirect_cond: int = 0
    redirect_indirect: int = 0
    # From SRAM the core does not redirect an unconditional branch early.
    redirect_uncond_sram: int = 0
    # A conditional branch that issued as the second of a pair redirects this much later when
    # taken: pairing saves its slot only when it falls through.
    redirect_paired: int = 1
    # A load's result is usable by an instruction issuing this many cycles after it. A multiply's
    # likewise.
    load_use: int = 2
    multiply_use: int = 2
    # A divide, measured at 100 / 7. The core stops early by operand size, so this is one point.
    divide: int = 5
    # Bus slots a data access to the code region holds, which fetch from there then cannot use.
    cahb_data_slots: int = 1
    # An access through the PPB waits for an S-AHB access this many cycles after it issued, and
    # an S-AHB access for a PPB one the same: the core keeps the two buses in order.
    ppb_after_sahb: int = 2
    sahb_after_ppb: int = 2
    # Extra cycles a fetch from flash costs; flash has its own accelerator, modelled crudely.
    flash_fetch: int = 0


@dataclass
class Insn:
    address: int
    size: int
    mnemonic: str
    reads: frozenset = frozenset()
    writes: frozenset = frozenset()
    loads: int = 0  # data reads
    stores: int = 0  # data writes
    branch: str = ""  # "", "uncond", "cond", "indirect"
    occupancy: int = 1
    # Registers a store writes out, which the core takes a cycle later than its other operands.
    store_data: frozenset = frozenset()


@dataclass
class Engine:
    params: Params = field(default_factory=Params)

    def __post_init__(self):
        self.reset()

    def reset(self) -> None:
        self.claims: dict[str, set[int]] = {C_AHB: set(), S_AHB: set()}
        self.fetch_word = None  # next word address the fetch unit will ask for
        self.fetch_free = 0  # earliest cycle that fetch may take the bus
        self.arrived: dict[int, int] = {}  # word address -> earliest issue cycle it allows
        self.order: list[int] = []  # words in the order they were fetched, for capacity
        self.consumed: dict[int, int] = {}  # word address -> cycle the last of it issued
        self.last_issue = -1
        self.last_occupancy = 1
        self.pending_pair = False  # the previous instruction may still take a partner
        self.paired = False  # the previous instruction issued as the second of a pair
        self.ready: dict[str, int] = {}
        self.last_sahb = -100
        self.last_ppb = -100
        self.current: Insn | None = None
        self.current_issue = 0
        self.accesses = 0
        self.loaded_now = False
        self.log: list[str] | None = None

    # -- fetch -------------------------------------------------------------
    def _claim_fetch(self, word: int, earliest: int) -> int:
        bus = bus_of(word)
        cycle = max(earliest, self.fetch_free)
        # The buffer holds queue_words words: the one that many back has to be gone first.
        depth = self.params.queue_words
        if len(self.order) >= depth:
            gone = self.consumed.get(self.order[-depth])
            if gone is not None:
                cycle = max(cycle, gone + self.params.queue_release)
        while cycle in self.claims.get(bus, ()):
            cycle += 1
        self.fetch_free = cycle + 1
        return cycle

    def _word_ready(self, word: int) -> int:
        """The earliest cycle an instruction in `word` can issue, fetching up to it if needed."""
        if word in self.arrived:
            return self.arrived[word]
        if self.fetch_word is None or word < self.fetch_word or word - self.fetch_word > 64:
            self.redirect(word, self.last_issue + 1)
        while self.fetch_word <= word:
            w = self.fetch_word
            slot = self._claim_fetch(w, 0)
            if bus_of(w) == S_AHB:
                latency = self.params.fetch_to_issue_sram
            else:
                latency = self.params.fetch_to_issue + (self.params.flash_fetch if w < 0x04000000 else 0)
            self.arrived[w] = slot + latency
            self.order.append(w)
            self.fetch_word = w + 4
        return self.arrived[word]

    def redirect(self, target: int, at: int) -> None:
        """Start fetching at `target` from cycle `at`, dropping whatever was buffered."""
        self.fetch_word = target & ~3
        self.fetch_free = max(at, 0)
        self.arrived = {}
        self.order = []

    # -- issue -------------------------------------------------------------
    def issue(self, insn: Insn, prev_taken_to: int | None = None) -> int:
        p = self.params
        prev = self.current
        if prev is not None and self.loaded_now:
            for reg in prev.writes:
                self.ready[reg] = self.current_issue + p.load_use
        self.loaded_now = False
        if prev is not None and prev_taken_to is not None:
            kind = prev.branch or "indirect"
            delta = {"uncond": p.redirect_uncond, "cond": p.redirect_cond}.get(kind, p.redirect_indirect)
            latency = p.fetch_to_issue_sram if bus_of(prev.address) == S_AHB else p.fetch_to_issue
            ready_self = self.arrived.get((prev.address + prev.size - 2) & ~3, self.current_issue)
            at = max(self.current_issue + delta + (p.redirect_paired if self.paired else 0), ready_self - latency + 1)
            if kind == "uncond" and bus_of(prev.address) == S_AHB:
                at = max(self.current_issue + p.redirect_uncond_sram, ready_self - latency + 1)
            self.redirect(prev_taken_to, at)

        first = self._word_ready(insn.address & ~3)
        last = self._word_ready((insn.address + insn.size - 2) & ~3)
        at = max(first, last)
        at = max(at, self.last_issue + self.last_occupancy)
        for reg in insn.reads:
            ready = self.ready.get(reg, 0)
            if reg in insn.store_data:
                ready -= 1
            at = max(at, ready)

        # A 16-bit instruction can take a partner: a 16-bit NOP, or a 16-bit conditional branch,
        # which the core issues predicted not taken. cbz and cbnz never pair.
        pairs = insn.size == 2 and (insn.mnemonic == "nop" or (insn.branch == "cond" and not insn.mnemonic.startswith("cb")))
        if (
            prev is not None
            and self.pending_pair
            and pairs
            and max(first, last) <= self.current_issue
        ):
            at = self.current_issue
            self.pending_pair = False
            self.paired = True
        else:
            self.paired = False
            self.pending_pair = insn.size == 2 and insn.branch == ""
            self.last_issue = at
            self.last_occupancy = insn.occupancy

        base = insn.mnemonic.split(".")[0]
        if base.startswith("it") and base[2:] and set(base[2:]) <= {"t", "e"} or base == "it":
            # IT folds into the instruction after it.
            self.last_occupancy = 0
        elif base in ("udiv", "sdiv"):
            self.last_occupancy = p.divide
        if base in ("mul", "muls", "mla", "mls", "smull", "umull", "smlal", "umlal"):
            for reg in insn.writes:
                self.ready[reg] = at + p.multiply_use
        for word in {insn.address & ~3, (insn.address + insn.size - 2) & ~3}:
            self.consumed[word] = at
        self.current = insn
        self.current_issue = at
        self.accesses = 0
        if self.log is not None:
            self.log.append(f"{insn.address:08x} {insn.mnemonic:<8} issue {at:4d}  words {first}/{last}")
        return at

    def access(self, address: int, write: bool) -> int:
        """The cycle the current instruction's next data access takes its bus."""
        p = self.params
        bus = bus_of(address)
        at = self.current_issue + self.accesses
        if bus == PPB:
            at = max(at, self.last_sahb + p.ppb_after_sahb)
            self.last_ppb = at
        elif bus == S_AHB:
            at = max(at, self.last_ppb + p.sahb_after_ppb)
        if at > self.current_issue + self.accesses:
            # The wait stalls the instruction itself and everything behind it.
            self.last_issue = at - self.accesses
            self.current_issue = at - self.accesses
        if bus != PPB:
            claims = self.claims[bus]
            while at in claims:
                at += 1
            claims.add(at)
            if bus == C_AHB:
                for extra in range(1, p.cahb_data_slots):
                    claims.add(at + extra)
            if bus == S_AHB:
                self.last_sahb = at
        if self.log is not None:
            self.log.append(f"           {'W' if write else 'R'} {address:08x} bus {bus} at {at}")
        self.accesses += 1
        if not write:
            self.loaded_now = True
        if self.accesses > 1:
            self.last_occupancy = max(self.last_occupancy, self.accesses)
        return at

