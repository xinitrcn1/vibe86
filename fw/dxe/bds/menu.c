/* BdsDxe: the boot menu (F11/F12 at the prompt): a popup of the boot options */
#include "bds.h"

int
boot_menu(void)
{
	boot_scan();
	int n = nboot_options + 1;		/* + "Enter Setup" */
	int sel = 0;
	unsigned w = 52, top = 6, left = (80 - w) / 2, bottom = top + 3 + (unsigned)n;
	ui_clear(A_NORMAL);
	ui_cursor(0);
	for (;;) {
		ui_box(top, left, bottom, left + w, A_POP, 1);
		ui_text(top + 1, left + 2, A_POP, "Please select boot device:");
		for (int i = 0; i < n; i++) {
			const char *d = i < nboot_options ? boot_options[i].desc : "Enter Setup";
			UINTN a = i == sel ? A_POPSEL : A_POP;
			ui_fill(top + 2 + (unsigned)i, left + 1, w - 1, ' ', a);
			ui_text(top + 2 + (unsigned)i, left + 2, a, "%.46s", d);
		}
		ui_text(bottom + 1, left, A_NORMAL, "  \x18 and \x19 to move selection");
		ui_text(bottom + 2, left, A_NORMAL, "  ENTER to select boot device");
		ui_text(bottom + 3, left, A_NORMAL, "  ESC to boot using defaults");
		EFI_INPUT_KEY k;
		ui_key(&k, 0);
		if (k.ScanCode == SCAN_UP && sel > 0)
			sel--;
		else if (k.ScanCode == SCAN_DOWN && sel < n - 1)
			sel++;
		else if (k.ScanCode == SCAN_ESC)
			return -1;
		else if (k.UnicodeChar == CHAR_CARRIAGE_RETURN) {
			ui_clear(A_NORMAL);
			return sel < nboot_options ? sel : -2;
		}
	}
}
