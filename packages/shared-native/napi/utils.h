#include <napi.h>
#include "hostmem/mono_timer.h"

namespace gradido::utils {

    class MonotonicTimer : public Napi::ObjectWrap<MonotonicTimer> {
    public:
        static Napi::Object Init(Napi::Env env, Napi::Object exports);
        
        MonotonicTimer(const Napi::CallbackInfo& info); 
        ~MonotonicTimer();

    private:
        Napi::Value Reset(const Napi::CallbackInfo& info);
        Napi::Value ToString(const Napi::CallbackInfo& info);

        hostmem_mono_timer mTimer;
    };
    
    Napi::Value DurationToString(const Napi::CallbackInfo& info);
} // namespace gradido::types