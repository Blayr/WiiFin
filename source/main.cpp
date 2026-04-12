#include "core/App.h"
#include <ogc/lwp.h>
#include <ogc/system.h>

int main(int argc, char* argv[]) {
    SYS_Report("[DBG] main thread self=%p\n", (void*)(uintptr_t)LWP_GetSelf());
    App app;
    app.init(argc > 0 ? argv[0] : nullptr);
    app.run();
    return 0;
}
