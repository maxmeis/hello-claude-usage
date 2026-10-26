#include "display_diagnostic.h"

/* This build has no monitor, account, or Wi-Fi entry point. */
void app_main(void)
{
    display_diagnostic_run();
}
