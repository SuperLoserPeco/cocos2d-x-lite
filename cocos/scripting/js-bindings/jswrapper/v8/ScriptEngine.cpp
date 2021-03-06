#include "ScriptEngine.hpp"

#if SCRIPT_ENGINE_TYPE == SCRIPT_ENGINE_V8

#include "Object.hpp"
#include "Class.hpp"
#include "Utils.hpp"
#include "../MappingUtils.hpp"

#if SE_ENABLE_INSPECTOR
#include "inspector_agent.h"
#include "env.h"
#include "node.h"
#endif

#define RETRUN_VAL_IF_FAIL(cond, val) \
    if (!(cond)) return val

namespace se {

    Class* __jsb_CCPrivateData_class = nullptr;

    namespace {
        ScriptEngine* __instance = nullptr;

        void __log(const v8::FunctionCallbackInfo<v8::Value>& info)
        {
            if (info[0]->IsString())
            {
                v8::String::Utf8Value utf8(info[0]);
                LOGD("JS: %s\n", *utf8);
            }
        }

        void __forceGC(const v8::FunctionCallbackInfo<v8::Value>& info)
        {
            ScriptEngine::getInstance()->garbageCollect();
        }

        std::string stackTraceToString(v8::Local<v8::StackTrace> stack)
        {
            std::string stackStr;
            char tmp[100] = {0};
            for (int i = 0, e = stack->GetFrameCount(); i < e; ++i)
            {
                v8::Local<v8::StackFrame> frame = stack->GetFrame(i);
                v8::Local<v8::String> script = frame->GetScriptName();
                std::string scriptName;
                if (!script.IsEmpty())
                {
                    scriptName = *v8::String::Utf8Value(script);
                }

                v8::Local<v8::String> func = frame->GetFunctionName();
                std::string funcName;
                if (!func.IsEmpty())
                {
                    funcName = *v8::String::Utf8Value(func);
                }

                stackStr += "[";
                snprintf(tmp, sizeof(tmp), "%d", i);
                stackStr += tmp;
                stackStr += "]";
                stackStr += (funcName.empty() ? "anonymous" : funcName.c_str());
                stackStr += "@";
                stackStr += (scriptName.empty() ? "(no filename)" : scriptName.c_str());
                stackStr += ":";
                snprintf(tmp, sizeof(tmp), "%d", frame->GetLineNumber());
                stackStr += tmp;

                if (i < (e-1))
                {
                    stackStr += "\n";
                }
            }

            return stackStr;
        }
    } // namespace {

    void ScriptEngine::onFatalErrorCallback(const char* location, const char* message)
    {
        std::string errorStr = "[FATAL ERROR] location: ";
        errorStr += location;
        errorStr += ", message: ";
        errorStr += message;

        LOGE("%s\n", errorStr.c_str());
        if (getInstance()->_exceptionCallback != nullptr)
        {
            getInstance()->_exceptionCallback(location, message, "(no stack information)");
        }
    }

    void ScriptEngine::onOOMErrorCallback(const char* location, bool is_heap_oom)
    {
        std::string errorStr = "[OOM ERROR] location: ";
        errorStr += location;
        std::string message;
        message = "is heap out of memory: ";
        if (is_heap_oom)
            message += "true";
        else
            message += "false";

        errorStr += ", " + message;
        LOGE("%s\n", errorStr.c_str());
        if (getInstance()->_exceptionCallback != nullptr)
        {
            getInstance()->_exceptionCallback(location, message.c_str(), "(no stack information)");
        }
    }

    void ScriptEngine::onMessageCallback(v8::Local<v8::Message> message, v8::Local<v8::Value> data)
    {
        ScriptEngine* thiz = getInstance();
        v8::Local<v8::String> msg = message->Get();
        Value msgVal;
        internal::jsToSeValue(v8::Isolate::GetCurrent(), msg, &msgVal);
        assert(msgVal.isString());
        v8::ScriptOrigin origin = message->GetScriptOrigin();
        Value resouceNameVal;
        internal::jsToSeValue(v8::Isolate::GetCurrent(), origin.ResourceName(), &resouceNameVal);
        Value line;
        internal::jsToSeValue(v8::Isolate::GetCurrent(), origin.ResourceLineOffset(), &line);
        Value column;
        internal::jsToSeValue(v8::Isolate::GetCurrent(), origin.ResourceColumnOffset(), &column);

        std::string location = resouceNameVal.toString() + ":" + line.toStringForce() + ":" + column.toStringForce();

        std::string errorStr = msgVal.toString() + ", " + location;
        std::string stackStr = stackTraceToString(message->GetStackTrace());
        if (!stackStr.empty())
        {
            if (line.toInt32() == 0)
            {
                location = "(see stack)";
            }
            errorStr += "\nSTACK:\n" + stackStr;
        }
        LOGE("ERROR: %s\n", errorStr.c_str());

        if (thiz->_exceptionCallback != nullptr)
        {
            thiz->_exceptionCallback(location.c_str(), msgVal.toString().c_str(), stackStr.c_str());
        }

        if (!thiz->_isErrorHandleWorking)
        {
            thiz->_isErrorHandleWorking = true;

            Value errorHandler;
            if (thiz->_globalObj->getProperty("__errorHandler", &errorHandler) && errorHandler.isObject() && errorHandler.toObject()->isFunction())
            {
                ValueArray args;
                args.push_back(resouceNameVal);
                args.push_back(line);
                args.push_back(msgVal);
                args.push_back(Value(stackStr));
                errorHandler.toObject()->call(args, thiz->_globalObj);
            }

            thiz->_isErrorHandleWorking = false;
        }
        else
        {
            LOGE("ERROR: __errorHandler has exception\n");
        }
    }

    void ScriptEngine::privateDataFinalize(void* nativeObj)
    {
        internal::PrivateData* p = (internal::PrivateData*)nativeObj;

        Object::nativeObjectFinalizeHook(p->data);

        assert(p->seObj->getRefCount() == 1);

        p->seObj->decRef();

        free(p);
    }

    ScriptEngine *ScriptEngine::getInstance()
    {
        if (__instance == nullptr)
        {
            __instance = new ScriptEngine();
        }

        return __instance;
    }

    void ScriptEngine::destroyInstance()
    {
        delete __instance;
        __instance = nullptr;
    }

    ScriptEngine::ScriptEngine()
    : _platform(nullptr)
    , _isolate(nullptr)
    , _handleScope(nullptr)
    , _allocator(nullptr)
    , _globalObj(nullptr)
    , _exceptionCallback(nullptr)
#if SE_ENABLE_INSPECTOR
    , _env(nullptr)
#endif
    , _debuggerServerPort(0)
    , _vmId(0)
    , _isValid(false)
    , _isGarbageCollecting(false)
    , _isInCleanup(false)
    , _isErrorHandleWorking(false)
    {
        //        RETRUN_VAL_IF_FAIL(v8::V8::InitializeICUDefaultLocation(nullptr, "/Users/james/Project/v8/out.gn/x64.debug/icudtl.dat"), false);
        //        v8::V8::InitializeExternalStartupData("/Users/james/Project/v8/out.gn/x64.debug/natives_blob.bin", "/Users/james/Project/v8/out.gn/x64.debug/snapshot_blob.bin"); //TODO
        _platform = v8::platform::CreateDefaultPlatform();
        v8::V8::InitializePlatform(_platform);
        bool ok = v8::V8::Initialize();
        assert(ok);
    }

    ScriptEngine::~ScriptEngine()
    {
        cleanup();
        v8::V8::Dispose();
        v8::V8::ShutdownPlatform();
        delete _platform;
        _platform = nullptr;
    }

    bool ScriptEngine::init()
    {
        cleanup();
        LOGD("Initializing V8, version: %s\n", v8::V8::GetVersion());
        ++_vmId;

        for (const auto& hook : _beforeInitHookArray)
        {
            hook();
        }
        _beforeInitHookArray.clear();

        assert(_allocator == nullptr);
        _allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
        // Create a new Isolate and make it the current one.
        _createParams.array_buffer_allocator = _allocator;
        _isolate = v8::Isolate::New(_createParams);
        v8::HandleScope hs(_isolate);
        _isolate->Enter();

        _isolate->SetCaptureStackTraceForUncaughtExceptions(true, 20, v8::StackTrace::kOverview);

        _isolate->SetFatalErrorHandler(onFatalErrorCallback);
        _isolate->SetOOMErrorHandler(onOOMErrorCallback);
        _isolate->AddMessageListener(onMessageCallback);

        _context.Reset(_isolate, v8::Context::New(_isolate));
        _context.Get(_isolate)->Enter();

        NativePtrToObjectMap::init();
        NonRefNativePtrCreatedByCtorMap::init();

        Class::setIsolate(_isolate);
        Object::setIsolate(_isolate);

        _globalObj = Object::_createJSObject(nullptr, _context.Get(_isolate)->Global());
        _globalObj->root();

        _globalObj->setProperty("scriptEngineType", se::Value("V8"));

        _globalObj->defineFunction("log", __log);
        _globalObj->defineFunction("forceGC", __forceGC);

        __jsb_CCPrivateData_class = Class::create("__CCPrivateData", _globalObj, nullptr, nullptr);
        __jsb_CCPrivateData_class->defineFinalizeFunction(privateDataFinalize);
        __jsb_CCPrivateData_class->setCreateProto(false);
        __jsb_CCPrivateData_class->install();

        _isValid = true;

        for (const auto& hook : _afterInitHookArray)
        {
            hook();
        }
        _afterInitHookArray.clear();

        return _isValid;
    }

    void ScriptEngine::cleanup()
    {
        if (!_isValid)
            return;

        LOGD("ScriptEngine::cleanup begin ...\n");
        _isInCleanup = true;

        {
            AutoHandleScope hs;
            for (const auto& hook : _beforeCleanupHookArray)
            {
                hook();
            }
            _beforeCleanupHookArray.clear();

            SAFE_DEC_REF(_globalObj);
            Object::cleanup();
            Class::cleanup();
            garbageCollect();

#if SE_ENABLE_INSPECTOR
            _env->inspector_agent()->Stop();

            node::FreeIsolateData(_isolateData);
            _env->CleanupHandles();
            node::FreeEnvironment(_env);
#endif

            _context.Get(_isolate)->Exit();
            _context.Reset();
            _isolate->Exit();
        }
        _isolate->Dispose();

        delete _allocator;
        _allocator = nullptr;
        _isolate = nullptr;
        _globalObj = nullptr;
        _isValid = false;

        _registerCallbackArray.clear();

        for (const auto& hook : _afterCleanupHookArray)
        {
            hook();
        }
        _afterCleanupHookArray.clear();

        _isInCleanup = false;
        NativePtrToObjectMap::destroy();
        NonRefNativePtrCreatedByCtorMap::destroy();

        LOGD("ScriptEngine::cleanup end ...\n");
    }

    Object* ScriptEngine::getGlobalObject() const
    {
        return _globalObj;
    }

    void ScriptEngine::addBeforeInitHook(const std::function<void()>& hook)
    {
        _beforeInitHookArray.push_back(hook);
    }

    void ScriptEngine::addAfterInitHook(const std::function<void()>& hook)
    {
        _afterInitHookArray.push_back(hook);
    }

    void ScriptEngine::addBeforeCleanupHook(const std::function<void()>& hook)
    {
        _beforeCleanupHookArray.push_back(hook);
    }

    void ScriptEngine::addAfterCleanupHook(const std::function<void()>& hook)
    {
        _afterCleanupHookArray.push_back(hook);
    }

    void ScriptEngine::addRegisterCallback(RegisterCallback cb)
    {
        assert(std::find(_registerCallbackArray.begin(), _registerCallbackArray.end(), cb) == _registerCallbackArray.end());
        _registerCallbackArray.push_back(cb);
    }

    bool ScriptEngine::start()
    {
        if (!init())
            return false;

        se::AutoHandleScope hs;

        // debugger
        if (isDebuggerEnabled())
        {
#if SE_ENABLE_INSPECTOR
            // V8 inspector stuff, most code are taken from NodeJS.
            _isolateData = node::CreateIsolateData(_isolate, uv_default_loop());
            _env = node::CreateEnvironment(_isolateData, _context.Get(_isolate), 0, nullptr, 0, nullptr);

            node::DebugOptions options;
            options.set_wait_for_connect(false); // Don't wait for connect, otherwise, the program will be hung up.
            options.set_inspector_enabled(true);
            options.set_port((int)_debuggerServerPort);
            options.set_host_name(_debuggerServerAddr.c_str());
            _env->inspector_agent()->Start(_platform, "", options);
#endif
        }
        //
        bool ok = false;
        _startTime = std::chrono::steady_clock::now();

        for (auto cb : _registerCallbackArray)
        {
            ok = cb(_globalObj);
            assert(ok);
            if (!ok)
                break;
        }

        // After ScriptEngine is started, _registerCallbackArray isn't needed. Therefore, clear it here.
        _registerCallbackArray.clear();

        return ok;
    }

    void ScriptEngine::garbageCollect()
    {
        LOGD("GC begin ..., (js->native map) size: %d, all objects: %d\n", (int)NativePtrToObjectMap::size(), (int)__objectMap.size());
        const double kLongIdlePauseInSeconds = 1.0;
        _isolate->ContextDisposedNotification();
        _isolate->IdleNotificationDeadline(_platform->MonotonicallyIncreasingTime() + kLongIdlePauseInSeconds);
        // By sending a low memory notifications, we will try hard to collect all
        // garbage and will therefore also invoke all weak callbacks of actually
        // unreachable persistent handles.
        _isolate->LowMemoryNotification();
        LOGD("GC end ..., (js->native map) size: %d, all objects: %d\n", (int)NativePtrToObjectMap::size(), (int)__objectMap.size());
    }

    bool ScriptEngine::isGarbageCollecting()
    {
        return _isGarbageCollecting;
    }

    void ScriptEngine::_setGarbageCollecting(bool isGarbageCollecting)
    {
        _isGarbageCollecting = isGarbageCollecting;
    }

    bool ScriptEngine::isValid() const
    {
        return _isValid;
    }

    bool ScriptEngine::evalString(const char* script, ssize_t length/* = -1 */, Value* ret/* = nullptr */, const char* fileName/* = nullptr */)
    {
        assert(script != nullptr);
        if (length < 0)
            length = strlen(script);

        if (fileName == nullptr)
            fileName = "(no filename)";

        // Fix the source url is too long displayed in Chrome debugger.
        std::string sourceUrl = fileName;
        static const std::string prefixKey = "/temp/quick-scripts/";
        size_t prefixPos = sourceUrl.find(prefixKey);
        if (prefixPos != std::string::npos)
        {
            sourceUrl = sourceUrl.substr(prefixPos + prefixKey.length());
        }

        std::string scriptStr(script, length);

        v8::MaybeLocal<v8::String> source = v8::String::NewFromUtf8(_isolate, scriptStr.c_str(), v8::NewStringType::kNormal);
        if (source.IsEmpty())
            return false;

        v8::MaybeLocal<v8::String> originStr = v8::String::NewFromUtf8(_isolate, sourceUrl.c_str(), v8::NewStringType::kNormal);
        if (originStr.IsEmpty())
            return false;

        v8::ScriptOrigin origin(originStr.ToLocalChecked());
        v8::MaybeLocal<v8::Script> maybeScript = v8::Script::Compile(_context.Get(_isolate), source.ToLocalChecked(), &origin);

        bool success = false;

        if (!maybeScript.IsEmpty())
        {
            v8::Local<v8::Script> v8Script = maybeScript.ToLocalChecked();
            v8::MaybeLocal<v8::Value> maybeResult = v8Script->Run(_context.Get(_isolate));

            if (!maybeResult.IsEmpty())
            {
                v8::Local<v8::Value> result = maybeResult.ToLocalChecked();

                if (!result->IsUndefined() && ret != nullptr)
                {
                    internal::jsToSeValue(_isolate, result, ret);
                }

                success = true;
            }
        }

//        assert(success);
        return success;
    }

    void ScriptEngine::setFileOperationDelegate(const FileOperationDelegate& delegate)
    {
        _fileOperationDelegate = delegate;
    }

    bool ScriptEngine::runScript(const std::string& path, Value* ret/* = nullptr */)
    {
        assert(!path.empty());
        assert(_fileOperationDelegate.isValid());

        std::string scriptBuffer = _fileOperationDelegate.onGetStringFromFile(path);

        if (!scriptBuffer.empty())
        {
            return evalString(scriptBuffer.c_str(), scriptBuffer.length(), ret, path.c_str());
        }

        LOGE("ScriptEngine::runScript script %s, buffer is empty!\n", path.c_str());
        return false;
    }

    void ScriptEngine::_retainScriptObject(void* owner, void* target)
    {
        auto iterOwner = NativePtrToObjectMap::find(owner);
        if (iterOwner == NativePtrToObjectMap::end())
        {
            return;
        }

        auto iterTarget = NativePtrToObjectMap::find(target);
        if (iterTarget == NativePtrToObjectMap::end())
        {
            return;
        }

        clearException();
        AutoHandleScope hs;
        iterOwner->second->attachObject(iterTarget->second);
    }

    void ScriptEngine::_releaseScriptObject(void* owner, void* target)
    {
        auto iterOwner = NativePtrToObjectMap::find(owner);
        if (iterOwner == NativePtrToObjectMap::end())
        {
            return;
        }

        auto iterTarget = NativePtrToObjectMap::find(target);
        if (iterTarget == NativePtrToObjectMap::end())
        {
            return;
        }

        clearException();
        AutoHandleScope hs;
        iterOwner->second->detachObject(iterTarget->second);
    }

    void ScriptEngine::clearException()
    {
        //FIXME:
    }

    void ScriptEngine::setExceptionCallback(const ExceptionCallback& cb)
    {
        _exceptionCallback = cb;
    }

    v8::Local<v8::Context> ScriptEngine::_getContext() const
    {
        return _context.Get(_isolate);
    }

    void ScriptEngine::enableDebugger(const std::string& serverAddr, uint32_t port)
    {
        _debuggerServerAddr = serverAddr;
        _debuggerServerPort = port;
    }

    bool ScriptEngine::isDebuggerEnabled() const
    {
        return !_debuggerServerAddr.empty() && _debuggerServerPort > 0;
    }

    void ScriptEngine::mainLoopUpdate()
    {
        // empty implementation
    }

} // namespace se {

#endif // #if SCRIPT_ENGINE_TYPE == SCRIPT_ENGINE_V8
