/************************************************************************
 ************************************************************************
    FAUST compiler
	Copyright (C) 2003-2004 GRAME, Centre National de Creation Musicale
    ---------------------------------------------------------------------
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 ************************************************************************
 ************************************************************************/
 
#include <stdio.h>
#include "llvm_dsp_aux.hh"
#include "libfaust.h"
        
void* llvm_dsp_factory::LoadOptimize(const std::string& function)
{
    llvm::Function* fun_ptr = fResult->fModule->getFunction(function);
    if (fun_ptr) {
        return fJIT->getPointerToFunction(fun_ptr);
    } else {
        throw -1;
    }
}

static Module* LoadModule(const std::string filename, LLVMContext& context)
{
    //printf("Load module : %s \n", filename.c_str());
    
    SMDiagnostic err;
    Module* res = ParseIRFile(filename, err, context);
    if (!res) {
    #if defined(LLVM_31) || defined(LLVM_32) 
        err.print("LoadModule", errs());
    #else
        err.Print("LoadModule", errs());
    #endif
    }
    return res;
}

LLVMResult* llvm_dsp_factory::CompileModule(int argc, const char *argv[], const char* library_path,  const char* draw_path, const char* input_name, const char* input, char* error_msg)
{
    //printf("Compile module...\n");
    
    int argc1 = argc + 3;
 	const char* argv1[32];
	assert(argc1 < 32);
    argv1[0] = "faust";
	argv1[1] = "-lang";
	argv1[2] = "llvm";
    for (int i = 0; i < argc; i++) {
        argv1[i+3] = argv[i];
        if (strcmp(argv[i], "-sch") == 0) {
            fScheduler = true;
        }
    }
    
    fLibraryPath = string(library_path);
    return compile_faust_llvm(argc1, argv1, library_path, draw_path, input_name, input, error_msg);
}

// Bitcode
std::string llvm_dsp_factory::writeDSPFactoryToBitcode()
{
    std::string res;
    raw_string_ostream out(res);
    WriteBitcodeToFile(fResult->fModule, out);
    out.flush();
    return res;
}

void llvm_dsp_factory::writeDSPFactoryToBitcodeFile(const std::string& bit_code_path)
{
    std::string err;
    raw_fd_ostream out(bit_code_path.c_str(), err, raw_fd_ostream::F_Binary);
    WriteBitcodeToFile(fResult->fModule, out);
}

// IR
string llvm_dsp_factory::writeDSPFactoryToIR()
{
    std::string res;
    raw_string_ostream out(res);
    PassManager PM;
    PM.add(createPrintModulePass(&out));
    PM.run(*fResult->fModule);
    out.flush();
    return res;
}

void llvm_dsp_factory::writeDSPFactoryToIRFile(const std::string& ir_code_path)
{
    std::string err;
    raw_fd_ostream out(ir_code_path.c_str(), err, raw_fd_ostream::F_Binary);
    PassManager PM;
    PM.add(createPrintModulePass(&out));
    PM.run(*fResult->fModule);
    out.flush();
}

llvm_dsp_factory::llvm_dsp_factory(Module* module, LLVMContext* context, const std::string& target, int opt_level)
{
    fOptLevel = opt_level;
    fTarget = target;
    Init();
    fResult = static_cast<LLVMResult*>(calloc(sizeof(LLVMResult), 0));
    fResult->fModule = module;
    fResult->fContext = context;
}

llvm_dsp_factory::llvm_dsp_factory(int argc, const char *argv[], 
                                    const std::string& library_path,
                                    const std::string& draw_path, 
                                    const std::string& name,
                                    const std::string& input, 
                                    const std::string& target, 
                                    char* error_msg, int opt_level)
{
    fOptLevel = opt_level;
    fTarget = target;
    Init();
    fResult = CompileModule(argc, argv, library_path.c_str(), draw_path.c_str(), name.c_str(), input.c_str(), error_msg);
}

void llvm_dsp_factory::Init()
{
    fJIT = 0;
    fNew = 0;
    fDelete = 0;
    fGetNumInputs = 0;
    fGetNumOutputs = 0;
    fBuildUserInterface = 0;
    fInit = 0;
    fClassInit = 0;
    fInstanceInit = 0;
    fCompute = 0;
    fScheduler = false;
}

llvm_dsp_aux* llvm_dsp_factory::createDSPInstance()
{
    assert(fResult->fModule);
    assert(fJIT);
    return new llvm_dsp_aux(this, fNew());
}

bool llvm_dsp_factory::initJIT()
{
    // First check is Faust compilation succeeded... (valid LLVM module)
    if (!fResult->fModule) {
        return false;
    }
    
    // Save current locale
    char* current_local = setlocale(LC_ALL, NULL);
    // Setup standard "C" local 
    // (workaround for a bug in bicode generation : http://lists.cs.uiuc.edu/pipermail/llvmbugs/2012-May/023530.html )
    setlocale(LC_ALL, "C");
    
    InitializeNativeTarget();
    
    if (fTarget != "") {
         fResult->fModule->setTargetTriple(fTarget);
    } else {
    #if defined(LLVM_31) || defined(LLVM_32)
        fResult->fModule->setTargetTriple(llvm::sys::getDefaultTargetTriple());
    #else
        fResult->fModule->setTargetTriple(llvm::sys::getHostTriple());
    #endif
    }

    std::string err;
    EngineBuilder builder(fResult->fModule);
    builder.setOptLevel(CodeGenOpt::Aggressive);
    builder.setEngineKind(EngineKind::JIT);
    builder.setUseMCJIT(true);
    builder.setMCPU(llvm::sys::getHostCPUName());
    
#ifndef LLVM_30
    TargetMachine* tm = builder.selectTarget();
#endif
    //tm->Options.PrintMachineCode = 1;
    /*
    SmallVector<std::string, 4> attrs;
    attrs.push_back("sse");
    attrs.push_back("sse2");
    attrs.push_back("sse3");
    attrs.push_back("enable-unsafe-fp-math");
    builder.setMAttrs(attrs);
    */
#ifdef LLVM_30
    fJIT = builder.create();
#else
    fJIT = builder.create(tm);
#endif
    if (!fJIT) {
        // Restore saved locale
        setlocale(LC_ALL, current_local);
        return false;
    }
    
    // Run static constructors.
    fJIT->runStaticConstructorsDestructors(false);
    
    fJIT->DisableLazyCompilation(true);
#ifdef LLVM_32
    fResult->fModule->setDataLayout(fJIT->getDataLayout()->getStringRepresentation());
#else
    fResult->fModule->setDataLayout(fJIT->getTargetData()->getStringRepresentation());
#endif
    //fResult->fModule->dump();

    // Set up the optimizer pipeline. Start with registering info about how the
    // target lays out data structures.
    PassManager pm;
    FunctionPassManager fpm(fResult->fModule);
#ifdef LLVM_32    
    // TODO
#else
    pm.add(new TargetData(*fJIT->getTargetData()));
    fpm.add(new TargetData(*fJIT->getTargetData()));
#endif
    // Link with "scheduler" code
    if (fScheduler) {
        LLVMContext context;
        Module* scheduler = LoadModule(fLibraryPath + "scheduler.ll", context);
        if (scheduler) {
            //scheduler->dump();
            if (Linker::LinkModules(fResult->fModule, scheduler, Linker::DestroySource, &err)) {
                printf("Cannot link scheduler module : %s\n", err.c_str());
            }
            delete scheduler;
        } else {
            printf("File scheduler.ll not found...\n");
        }
    }
    
    // Taken from LLVM Opt.cpp
    PassManagerBuilder Builder;
    Builder.OptLevel = fOptLevel;

    if (fOptLevel > 1) {
        unsigned threshold = 225;
        if (fOptLevel > 2) {
            threshold = 275;
        }
        Builder.Inliner = createFunctionInliningPass(threshold);
    } else {
        Builder.Inliner = createAlwaysInlinerPass();
    }
    
    // We use '4' to activate de auto-vectorizer
    if (fOptLevel > 3) {
    #if defined(LLVM_32) 
        Builder.LoopVectorize = true;
        //Builder.Vectorize = true;
    #elif defined(LLVM_31)
        Builder.Vectorize = true;
    #endif
    }
      
    Builder.DisableUnrollLoops = (fOptLevel == 0);
    Builder.populateFunctionPassManager(fpm);
    Builder.populateModulePassManager(pm);
    
    pm.run(*fResult->fModule);
    
    //fResult->fModule->dump();
    
    try {
        fNew = (newDspFun)LoadOptimize("new_mydsp");
        fDelete = (deleteDspFun)LoadOptimize("delete_mydsp");
        fGetNumInputs = (getNumInputsFun)LoadOptimize("getNumInputs_mydsp");
        fGetNumOutputs = (getNumOutputsFun)LoadOptimize("getNumOutputs_mydsp");
        fBuildUserInterface = (buildUserInterfaceFun)LoadOptimize("buildUserInterface_mydsp");
        fInit = (initFun)LoadOptimize("init_mydsp");
        fClassInit = (classInitFun)LoadOptimize("classInit_mydsp");
        fInstanceInit = (instanceInitFun)LoadOptimize("instanceInit_mydsp");
        fCompute = (computeFun)LoadOptimize("compute_mydsp");
        fMetadata = (metadataFun)LoadOptimize("metadata_mydsp");
        // Restore saved locale
        setlocale(LC_ALL, current_local);
        return true;
    } catch (...) { // Module does not contain the Faust entry points...
        // Restore saved locale
        setlocale(LC_ALL, current_local);
        return false;
    }
}

llvm_dsp_factory::~llvm_dsp_factory()
{
    if (fJIT) {
        fJIT->runStaticConstructorsDestructors(true);
        // fResult->fModule is kept and deleted by fJIT
        delete fJIT;
    }
    
    delete fResult->fContext;
    free(fResult);
}

void llvm_dsp_factory::metadataDSPFactory(Meta* meta)
{
    MetaGlue glue;
    buildMetaGlue(&glue, meta);
    fMetadata(&glue);
}
  
// Instance 

llvm_dsp_aux::llvm_dsp_aux(llvm_dsp_factory* factory, llvm_dsp_imp* dsp)
    :fDSPFactory(factory), fDSP(dsp)
{
    assert(fDSPFactory);
    assert(fDSP);
}
        
llvm_dsp_aux::~llvm_dsp_aux()
{   
    if (fDSP) {
        fDSPFactory->fDelete(fDSP);
    }
}

int llvm_dsp_aux::getNumInputs()
{
    return fDSPFactory->fGetNumInputs(fDSP);
}
int llvm_dsp_aux::getNumOutputs()
{
    return fDSPFactory->fGetNumOutputs(fDSP);
}

void llvm_dsp_aux::classInit(int samplingFreq)
{
    fDSPFactory->fClassInit(samplingFreq);
}

void llvm_dsp_aux::instanceInit(int samplingFreq)
{
    fDSPFactory->fInstanceInit(fDSP, samplingFreq);
}

void llvm_dsp_aux::init(int samplingFreq)
{
    fDSPFactory->fInit(fDSP, samplingFreq);
}

void llvm_dsp_aux::buildUserInterface(UI* interface)
{
    UIGlue glue;
    buildUIGlue(&glue, interface);
    fDSPFactory->fBuildUserInterface(fDSP, &glue);
}

void llvm_dsp_aux::compute(int count, FAUSTFLOAT** input, FAUSTFLOAT** output)
{
    AVOIDDENORMALS;
    fDSPFactory->fCompute(fDSP, count, input, output);
}

static llvm_dsp_factory* CheckDSPFactory(llvm_dsp_factory* factory)
{
    if (factory->initJIT()) {
        return factory;
    } else {
        delete factory;
        return 0;
    }
}

// Public API

EXPORT llvm_dsp_factory* createDSPFactory(int argc, const char *argv[], 
    const std::string& library_path, const std::string& draw_path, const std::string& name, 
    const std::string& input, const std::string& target, 
    char* error_msg, int opt_level)
{
    return CheckDSPFactory(new llvm_dsp_factory(argc, argv, library_path, draw_path, name, input, target, error_msg, opt_level));
}
    
// Bitcode <==> string
EXPORT llvm_dsp_factory* readDSPFactoryFromBitcode(const std::string& bit_code, const std::string& target, int opt_level)
{
    string error_msg;
    MemoryBuffer* buffer = MemoryBuffer::getMemBuffer(StringRef(bit_code));
    LLVMContext* context = new LLVMContext();
    Module* module = ParseBitcodeFile(buffer, *context, &error_msg);
    delete buffer;
    
    if (module) {
        return CheckDSPFactory(new llvm_dsp_factory(module, context, target, opt_level));
    } else {
        printf("readDSPFactoryFromBitcode failed : %s\n", error_msg.c_str());
        return 0;
    }
}

EXPORT std::string writeDSPFactoryToBitcode(llvm_dsp_factory* factory)
{
    return factory->writeDSPFactoryToBitcode();
}

// Bitcode <==> file
EXPORT llvm_dsp_factory* readDSPFactoryFromBitcodeFile(const std::string& bit_code_path, const std::string& target, int opt_level)
{
    OwningPtr<MemoryBuffer> buffer;
    if (error_code ec = MemoryBuffer::getFileOrSTDIN(bit_code_path.c_str(), buffer)) {
        printf("readDSPFactoryFromBitcodeFile failed : %s\n", ec.message().c_str());
        return 0;
    }
  
    std::string error_msg;
    LLVMContext* context = new LLVMContext();
    Module* module = ParseBitcodeFile(buffer.get(), *context, &error_msg);
    
    if (module) {
        return CheckDSPFactory(new llvm_dsp_factory(module, context, target, opt_level));
    } else {
        printf("readDSPFactoryFromBitcodeFile failed : %s\n", error_msg.c_str());
        return 0;
    }
}

EXPORT void writeDSPFactoryToBitcodeFile(llvm_dsp_factory* factory, const std::string& bit_code_path)
{
    factory->writeDSPFactoryToBitcodeFile(bit_code_path);
}

// IR <==> string
EXPORT llvm_dsp_factory* readDSPFactoryFromIR(const std::string& ir_code, const std::string& target, int opt_level)
{
    SMDiagnostic err;
    MemoryBuffer* buffer = MemoryBuffer::getMemBuffer(StringRef(ir_code));
    LLVMContext* context = new LLVMContext();
    Module* module = ParseIR(buffer, err, *context); // ParseIR takes ownership of the given buffer, so don't delete it
    
    if (module) {
        return CheckDSPFactory(new llvm_dsp_factory(module, context, target, opt_level));
    } else {
    #if defined(LLVM_31) || defined(LLVM_32) 
        err.print("readDSPFactoryFromIR failed :", errs());
    #else
        err.Print("readDSPFactoryFromIR failed :", errs());
    #endif
        return 0;
    }
}

EXPORT std::string writeDSPFactoryToIR(llvm_dsp_factory* factory)
{
    return factory->writeDSPFactoryToIR();
}

// IR <==> file
EXPORT llvm_dsp_factory* readDSPFactoryFromIRFile(const std::string& ir_code_path, const std::string& target, int opt_level)
{
    SMDiagnostic err;
    LLVMContext* context = new LLVMContext();
    Module* module = ParseIRFile(ir_code_path, err, *context);
    
    if (module) {
        return CheckDSPFactory(new llvm_dsp_factory(module, context, target, opt_level));
    } else {
    #if defined(LLVM_31) || defined(LLVM_32) 
        err.print("readDSPFactoryFromIR failed :", errs());
    #else
        err.Print("readDSPFactoryFromIR failed :", errs());
    #endif
        return 0;
    }
}

EXPORT void writeDSPFactoryToIRFile(llvm_dsp_factory* factory, const std::string& ir_code_path)
{
    factory->writeDSPFactoryToIRFile(ir_code_path);
}

EXPORT void metadataDSPFactory(llvm_dsp_factory* factory, Meta* m)
{
    factory->metadataDSPFactory(m);
}

// Instance

EXPORT llvm_dsp* createDSPInstance(llvm_dsp_factory* factory)
{
    return reinterpret_cast<llvm_dsp*>(factory->createDSPInstance());
}

EXPORT void deleteDSPFactory(llvm_dsp_factory* factory) { delete factory; }

EXPORT void deleteDSPInstance(llvm_dsp* dsp) 
{
    delete dsp; 
}

EXPORT int llvm_dsp::getNumInputs()
{
    return reinterpret_cast<llvm_dsp_aux*>(this)->getNumInputs();
}

int EXPORT llvm_dsp::getNumOutputs()
{
    return reinterpret_cast<llvm_dsp_aux*>(this)->getNumOutputs();
}

EXPORT void llvm_dsp::classInit(int samplingFreq)
{
    reinterpret_cast<llvm_dsp_aux*>(this)->classInit(samplingFreq);
}

EXPORT void llvm_dsp::instanceInit(int samplingFreq)
{
    reinterpret_cast<llvm_dsp_aux*>(this)->instanceInit(samplingFreq);
}

EXPORT void llvm_dsp::init(int samplingFreq)
{
    reinterpret_cast<llvm_dsp_aux*>(this)->init(samplingFreq);
}

EXPORT void llvm_dsp::buildUserInterface(UI* interface)
{
    reinterpret_cast<llvm_dsp_aux*>(this)->buildUserInterface(interface);
}

EXPORT void llvm_dsp::compute(int count, FAUSTFLOAT** input, FAUSTFLOAT** output)
{
    reinterpret_cast<llvm_dsp_aux*>(this)->compute(count, input, output);
}