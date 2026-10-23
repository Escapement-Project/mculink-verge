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


_REGISTER_IDS: dict[str, int] = {}


def register_id(name: str) -> int:
    """Assign each decoded architectural register a compact process-local index."""
    index = _REGISTER_IDS.get(name)
    if index is None:
        index = len(_REGISTER_IDS)
        _REGISTER_IDS[name] = index
    return index


# Buses, by the address being fetched or accessed.
C_AHB, S_AHB, PPB = "c", "s", "p"


def bus_of(address: int) -> str:
    if address < 0x20000000:
        return C_AHB
    if address >= 0xE0000000:
        return PPB
    return S_AHB


@dataclass(slots=True)
class Params:
    # From a fetch's bus slot to the cycle its word can issue an instruction, decode included.
    fetch_to_issue: int = 2
    # The same from SRAM over S-AHB, which a branch shows and straight-line code hides.
    fetch_to_issue_sram: int = 2
    # The prefetch buffer, in halfwords: a word may be fetched once every halfword more than this
    # many before its end has issued, plus queue_release cycles. Counted in words instead when
    # queue_halfwords is None. Five halfwords is what makes a literal load from SRAMX cost a cycle,
    # as the bench measured, and also fits SRAM better than any whole number of words.
    queue_words: int = 3
    queue_release: int = 0
    queue_halfwords: int | None = 5
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


@dataclass(slots=True)
class Insn:
    address: int
    size: int
    mnemonic: str
    reads: frozenset = frozenset()
    writes: frozenset = frozenset()
    loads: int = 0  # data reads
    stores: int = 0  # data writes
    branch: str = ""  # "", "uncond", "cond", "indirect"
    branch_target: int | None = None
    occupancy: int = 1
    # Registers a store writes out, which the core takes a cycle later than its other operands.
    store_data: frozenset = frozenset()
    pairable: bool = field(init=False, repr=False, compare=False)
    folds_it: bool = field(init=False, repr=False, compare=False)
    is_divide: bool = field(init=False, repr=False, compare=False)
    is_multiply: bool = field(init=False, repr=False, compare=False)
    read_delays: tuple[tuple[int, bool], ...] = field(init=False, repr=False, compare=False)
    write_ids: tuple[int, ...] = field(init=False, repr=False, compare=False)
    register_limit: int = field(init=False, repr=False, compare=False)

    def __post_init__(self) -> None:
        self.read_delays = tuple((register_id(reg), reg in self.store_data) for reg in self.reads)
        self.write_ids = tuple(register_id(reg) for reg in self.writes)
        self.register_limit = max(
            [index for index, _ in self.read_delays] + list(self.write_ids), default=-1
        )
        base = self.mnemonic.split(".", 1)[0]
        self.pairable = self.size == 2 and (
            self.mnemonic == "nop" or (self.branch == "cond" and not self.mnemonic.startswith("cb"))
        )
        self.folds_it = (
            base.startswith("it") and bool(base[2:]) and set(base[2:]) <= {"t", "e"}
        ) or base == "it"
        self.is_divide = base in ("udiv", "sdiv")
        self.is_multiply = base in ("mul", "muls", "mla", "mls", "smull", "umull", "smlal", "umlal")


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
        self.hw_consumed: dict[int, int] = {}  # halfword address -> cycle it issued
        self.stream_start = 0
        self.last_issue = -1
        self.last_occupancy = 1
        self.pending_pair = False  # the previous instruction may still take a partner
        self.paired = False  # the previous instruction issued as the second of a pair
        self.ready = [0] * 64
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
        cycle = earliest if earliest > self.fetch_free else self.fetch_free
        if self.params.queue_halfwords is not None:
            # Everything before the last queue_halfwords halfwords up to this word's end has to
            # have issued, the newest of them being the halfword just below that point.
            depth, release = self.params.queue_halfwords, self.params.queue_release
            last = word + 4 - 2 * depth - 2
            if last >= self.stream_start:
                gone = self.hw_consumed.get(last)
                if gone is not None:
                    ready = gone + release
                    if ready > cycle:
                        cycle = ready
        else:
            # The buffer holds queue_words words: the one that many back has to be gone first.
            depth = self.params.queue_words
            if len(self.order) >= depth:
                gone = self.consumed.get(self.order[-depth])
                if gone is not None:
                    ready = gone + self.params.queue_release
                    if ready > cycle:
                        cycle = ready
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
        self.stream_start = target
        self.fetch_word = target & ~3
        self.fetch_free = at if at > 0 else 0
        self.arrived = {}
        self.order = []

    # -- issue -------------------------------------------------------------
    def issue(self, insn: Insn, prev_taken_to: int | None = None) -> int:
        p = self.params
        if insn.register_limit >= len(self.ready):
            self.ready.extend([0] * (insn.register_limit + 1 - len(self.ready)))
        prev = self.current
        if prev is not None and self.loaded_now:
            for reg in prev.write_ids:
                self.ready[reg] = self.current_issue + p.load_use
        self.loaded_now = False
        if prev is not None and prev_taken_to is not None:
            kind = prev.branch or "indirect"
            if kind == "uncond":
                delta = p.redirect_uncond
            elif kind == "cond":
                delta = p.redirect_cond
            else:
                delta = p.redirect_indirect
            prev_bus = bus_of(prev.address)
            latency = p.fetch_to_issue_sram if prev_bus == S_AHB else p.fetch_to_issue
            ready_self = self.arrived.get((prev.address + prev.size - 2) & ~3, self.current_issue)
            redirect = self.current_issue + delta + (p.redirect_paired if self.paired else 0)
            ready = ready_self - latency + 1
            at = redirect if redirect > ready else ready
            if kind == "uncond" and prev_bus == S_AHB:
                redirect = self.current_issue + p.redirect_uncond_sram
                at = redirect if redirect > ready else ready
            self.redirect(prev_taken_to, at)

        first_word = insn.address & ~3
        last_word = (insn.address + insn.size - 2) & ~3
        first = self._word_ready(first_word)
        last = first if last_word == first_word else self._word_ready(last_word)
        at = first if first > last else last
        ready = self.last_issue + self.last_occupancy
        if ready > at:
            at = ready
        for reg, store_data in insn.read_delays:
            ready = self.ready[reg]
            if store_data:
                ready -= 1
            if ready > at:
                at = ready

        # A 16-bit instruction can take a partner: a 16-bit NOP, or a 16-bit conditional branch,
        # which the core issues predicted not taken. cbz and cbnz never pair.
        if (
            prev is not None
            and self.pending_pair
            and insn.pairable
            and (first if first > last else last) <= self.current_issue
        ):
            at = self.current_issue
            self.pending_pair = False
            self.paired = True
        else:
            self.paired = False
            self.pending_pair = insn.size == 2 and insn.branch == ""
            self.last_issue = at
            self.last_occupancy = insn.occupancy

        if insn.folds_it:
            # IT folds into the instruction after it.
            self.last_occupancy = 0
        elif insn.is_divide:
            self.last_occupancy = p.divide
        if insn.is_multiply:
            for reg in insn.write_ids:
                self.ready[reg] = at + p.multiply_use
        self.consumed[first_word] = at
        if last_word != first_word:
            self.consumed[last_word] = at
        self.hw_consumed[insn.address] = at
        if insn.size > 2:
            for half in range(insn.address + 2, insn.address + insn.size, 2):
                self.hw_consumed[half] = at
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
            ready = self.last_sahb + p.ppb_after_sahb
            if ready > at:
                at = ready
            self.last_ppb = at
        elif bus == S_AHB:
            ready = self.last_ppb + p.sahb_after_ppb
            if ready > at:
                at = ready
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
            if self.accesses > self.last_occupancy:
                self.last_occupancy = self.accesses
        return at

