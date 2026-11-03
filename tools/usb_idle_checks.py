"""Schedule wakeups around CPU 0's linked idle path."""

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS, Cs
from cycles import SCRATCH, u32
from energy_checks import machine
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_BASEPRI, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_PRIMASK, UC_ARM_REG_R0

MAILBOX_BASE = 0x4008B000


class Mailbox:
    """The two cores' mailbox registers, IRQ, IRQSET and IRQCLR, with the set and clear acting on
    IRQ as the hardware's do."""

    def __init__(self, probe):
        self.irq = [0, 0]
        probe.uc.mem_unmap(MAILBOX_BASE, 0x1000)
        probe.uc.mmio_map(MAILBOX_BASE, 0x1000, self.read, None, self.write, None)

    def read(self, uc, offset, size, data):
        core, register = divmod(offset, 16)
        return self.irq[core] if core < 2 and register == 0 else 0

    def write(self, uc, offset, size, value, data):
        core, register = divmod(offset, 16)
        if core >= 2:
            return
        if register == 0:
            self.irq[core] = value
        elif register == 4:
            self.irq[core] |= value
        elif register == 8:
            self.irq[core] &= ~value


def check_idle(c, elf, symbols, code_lab=False):
    # "mailbox-before-wait": CPU 1 raises the mailbox after the loop's tasks have looked for its work
    # and before the loop waits, as when it frees a request slot after arm_rx found none free.
    wakes = ["usb-before-mask", "irq-in-wait", "mailbox-in-wait", "mailbox-before-wait", "vcom-in-wait",
             "edges-in-wait"]
    if code_lab:
        wakes.append("aux-before-hammer")
    for wake in wakes:
        probe = machine(elf, symbols)
        mailbox = Mailbox(probe)
        serve, length = symbols["serve"]
        decoder = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
        decoder.skipdata = True
        masks = {ins.address for ins in decoder.disasm(bytes(probe.uc.mem_read(serve, length)), serve)
                 if ins.mnemonic == "msr" and ins.op_str.startswith("basepri,")}
        c.check(bool(masks), "CPU 0 idle check found no BASEPRI instruction in serve")
        state = {"queued": False, "injected": False, "tasks": 0, "waits": 0, "drained": False}
        skipped = {probe.sym[name] for name in ("energy_control_task", "energy_range_retry_task", "dio_edges_task",
                                              "aux_task", "reboot_poll") if name in probe.sym}

        def hook(uc, at, size, data):
            result = None
            if at in masks and not state["injected"] and wake == "usb-before-mask":
                # The IRQ finishes just before masking: the software queue is ready, no NVIC bit remains.
                c.check(uc.reg_read(UC_ARM_REG_BASEPRI) == 0, "USB race injection ran with interrupts already masked")
                state["queued"] = state["injected"] = True
            if at == probe.sym["tud_task_ext"]:
                state["tasks"] += 1
                if state["tasks"] == 2:
                    state["drained"] = True
                    uc.emu_stop()
                result = 0
            elif at == probe.sym["tud_task_event_ready"]:
                result = int(state["queued"])
            elif at == probe.sym["usb_speed_task"]:
                if wake == "mailbox-before-wait" and state["tasks"] == 1:
                    mailbox.irq[1] |= 1
                    state["injected"] = True
            elif at == probe.sym["vcom_ready"]:
                result = int(wake == "vcom-in-wait" and state["waits"] != 0)
            elif at == probe.sym["dio_edges_waiting"]:
                result = int(wake == "edges-in-wait" and state["waits"] != 0)
            elif at == probe.sym["wait_for_work"]:
                c.check(uc.reg_read(UC_ARM_REG_BASEPRI) == 0x20 and uc.reg_read(UC_ARM_REG_PRIMASK) == 0,
                        f"idle {wake}: wait must mask ordinary IRQs and leave priority-zero DIO enabled")
                state["waits"] += 1
                if wake not in ("usb-before-mask", "mailbox-before-wait"):
                    state["injected"] = True
                if wake == "irq-in-wait":
                    uc.mem_write(0xe000e100, u32(1))
                    uc.mem_write(0xe000e200, u32(1))
                elif wake == "mailbox-in-wait":
                    mailbox.irq[1] |= 1
            elif at == probe.sym["aux_has_work"]:
                result = int(wake == "aux-before-hammer")
            elif at in skipped:
                result = 0
            if result is not None:
                uc.reg_write(UC_ARM_REG_R0, result)
                uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

        probe.uc.reg_write(UC_ARM_REG_BASEPRI, 0)
        if wake == "aux-before-hammer":
            mailbox.irq[1] = SCRATCH | 2
            state["injected"] = True
        # Startup initializes the reboot sentinels.
        for name in ("usb_pending", "active_mode"):
            if name in probe.sym:
                probe.uc.mem_write(probe.sym[name], b"\xff")
        probe.uc.hook_add(UC_HOOK_CODE, hook)
        probe.uc.ctl_flush_tb()
        probe.call("serve", timeout_insns=4000)
        c.check(state["injected"] and state["drained"], f"idle {wake}: loop did not service work within 4000 instructions ({state})")
        c.check(probe.uc.reg_read(UC_ARM_REG_BASEPRI) == 0, f"idle {wake}: BASEPRI was not restored")
    c.note("CPU 0 idle: USB IRQ completes before masking; pending IRQ, mailbox, VCOM and DIO edges wake the SRAM wait;"
           " a mailbox raised after the tasks looked is not lost")


def check_idle_rearm(c, elf, symbols, offsets):
    """A re-arm waiting for the gap between pairs keeps CPU 0's loop from waiting while the
    conversion timer runs, and lets it wait while a hold has the timer stopped (src/main.c)."""
    ctimer2_tcr = 0x40028004
    for running in (True, False):
        probe = machine(elf, symbols)
        shared = symbols["dap_shared"][0]
        for name in ("usb_pending", "active_mode"):
            if name in probe.sym:
                probe.uc.mem_write(probe.sym[name], b"\xff")
        for name in ("energy_range_retry_pending", "energy_range_retry_enabled"):
            probe.uc.mem_write(shared + offsets[name], b"\x01")
        probe.uc.mem_write(ctimer2_tcr, u32(int(running)))
        state = {"tasks": 0, "waits": 0}
        stubbed = {probe.sym[name] for name in ("energy_control_task", "energy_range_retry_task", "dio_edges_task",
                                               "aux_task", "reboot_poll", "tud_task_event_ready", "vcom_ready",
                                               "dio_edges_waiting", "aux_has_work", "lab_hammer") if name in probe.sym}

        def hook(uc, at, size, data):
            if at == probe.sym["tud_task_ext"]:
                state["tasks"] += 1
                if state["tasks"] == 3:
                    uc.emu_stop()
            elif at == probe.sym["wait_for_work"]:
                state["waits"] += 1
                uc.emu_stop()
            elif at not in stubbed:
                return
            uc.reg_write(UC_ARM_REG_R0, 0)
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

        probe.uc.reg_write(UC_ARM_REG_BASEPRI, 0)
        probe.uc.hook_add(UC_HOOK_CODE, hook)
        probe.uc.ctl_flush_tb()
        probe.call("serve", timeout_insns=4000)
        if running:
            c.check(state["waits"] == 0 and state["tasks"] == 3,
                    f"idle with a re-arm waiting and the timer running: {state}")
        else:
            c.check(state["waits"] == 1, f"idle with a re-arm waiting through a hold: {state}")
    c.note("CPU 0 idle: a re-arm waiting for the gap keeps the loop awake while the conversion timer runs, not through a hold")


def check_idle_serviced(c, elf, symbols):
    """A raise the loop has serviced does not keep it from waiting: the pass clears the mailbox, so
    the wait that follows starts with it clear (src/main.c, serve)."""
    probe = machine(elf, symbols)
    mailbox = Mailbox(probe)
    for name in ("usb_pending", "active_mode"):
        if name in probe.sym:
            probe.uc.mem_write(probe.sym[name], b"\xff")
    mailbox.irq[1] = 1
    state = {"tasks": 0, "waits": 0, "raised_at_wait": None}
    stubbed = {probe.sym[name] for name in ("energy_control_task", "energy_range_retry_task", "dio_edges_task",
                                           "aux_task", "reboot_poll", "tud_task_event_ready", "vcom_ready",
                                           "dio_edges_waiting", "aux_has_work", "lab_hammer") if name in probe.sym}

    def hook(uc, at, size, data):
        if at == probe.sym["tud_task_ext"]:
            state["tasks"] += 1
            if state["tasks"] == 3:
                uc.emu_stop()
        elif at == probe.sym["wait_for_work"]:
            state["waits"] += 1
            state["raised_at_wait"] = mailbox.irq[1] & 1
            uc.emu_stop()
        elif at not in stubbed:
            return
        uc.reg_write(UC_ARM_REG_R0, 0)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    probe.uc.reg_write(UC_ARM_REG_BASEPRI, 0)
    probe.uc.hook_add(UC_HOOK_CODE, hook)
    probe.uc.ctl_flush_tb()
    probe.call("serve", timeout_insns=4000)
    c.check(state["waits"] == 1 and state["tasks"] == 1 and state["raised_at_wait"] == 0,
            f"idle after a serviced raise: {state}")
    c.note("CPU 0 idle: a pass clears a raise it serviced, and the loop then waits")
