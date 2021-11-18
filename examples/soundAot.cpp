#include "daScript/daScript.h"

using namespace das;

#define APP_NAME   "/modules/dasSound/examples/sound_example.das"

#define USE_AOT         1
#define LOG_AOT_ERRORS  1

void application () {
    TextPrinter tout;
    ModuleGroup dummyLibGroup;
    CodeOfPolicies policies;
    policies.aot = USE_AOT;
    policies.fail_on_no_aot = false;
    auto fAccess = make_smart<FsFileAccess>();
    auto program = compileDaScript(getDasRoot() + APP_NAME, fAccess, tout, dummyLibGroup, false, policies);
    if ( program->failed() ) {
        tout << "failed to compile\n";
        for ( auto & err : program->errors ) {
            tout << reportError(err.at, err.what, err.extra, err.fixme, err.cerr );
        }
        return;
    }
    Context ctx(program->getContextStackSize());
    if ( !program->simulate(ctx, tout) ) {
        tout << "failed to simulate\n";
        for ( auto & err : program->errors ) {
            tout << reportError(err.at, err.what, err.extra, err.fixme, err.cerr );
        }
        return;
    }
    if ( LOG_AOT_ERRORS && program->aotErrors.size() ) {
        tout << "AOT linker failed:\n\n";
        for ( auto & err : program->aotErrors ) {
            tout << err.what << "\n" << err.extra << "\n\n";
        }
        tout << "Total " << program->aotErrors.size() << " linking errors.\n";
    }
    auto fnTest = ctx.findFunction("main");
    if ( !fnTest ) {
        tout << "function 'main' not found\n";
        return;
    }
    if ( !verifyCall<void>(fnTest->debugInfo, dummyLibGroup) ) {
        tout << "function 'main', call arguments do not match. expecting def main : void\n";
        return;
    }
    ctx.eval(fnTest, nullptr);
    if ( auto ex = ctx.getException() ) {       // if function cased panic, report it
        tout << "exception: " << ex << "\n";
        return;
    }
}

int main( int, char * [] ) {
    NEED_ALL_DEFAULT_MODULES;
    NEED_MODULE(Module_UriParser);
    NEED_MODULE(Module_JobQue);
    #include "modules/external_need.inc"
    Module::Initialize();
    application();
    Module::Shutdown();
    return 0;
}
