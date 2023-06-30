
// Host Controller Doorbell Registers
#define XHCI_DOORBELL(n)				(0x0000 + (4 * (n)))
#define XHCI_DOORBELL_TARGET(x)			((x) & 0xff)
#define XHCI_DOORBELL_TARGET_GET(x)		((x) & 0xff)
#define XHCI_DOORBELL_STREAMID(x)		(((x) & 0xffff) << 16)
#define XHCI_DOORBELL_STREAMID_GET(x)	(((x) >> 16) & 0xffff)

