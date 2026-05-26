/* 36-slot inventory; the first 9 slots are the hotbar. */
#ifndef INVENTORY_H
#define INVENTORY_H

#define INV_HOTBAR 9
#define INV_TOTAL  36

typedef struct { int item; int count; } Slot;

extern Slot g_inv[INV_TOTAL];
extern int  g_invSel;        /* selected hotbar index 0..8 */

void InventoryInit(void);
int  InventoryAdd(int item, int count);   /* returns leftover that didn't fit */
void InventorySelect(int i);
void InventoryScroll(int dir);            /* dir +1 / -1 wraps the hotbar */
int  InventorySelectedItem(void);
void InventoryConsumeSelected(void);      /* remove one of the selected stack */

#endif /* INVENTORY_H */
