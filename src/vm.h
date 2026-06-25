#ifndef FERMIHV_VM_H
#define FERMIHV_VM_H

/* Configure HCR_EL2 and eret into the bare EL1 guest. Does not return:
 * control re-enters EL2 only via traps (HVC, aborts, etc.). */
void vm_run_guest(void);

#endif /* FERMIHV_VM_H */
