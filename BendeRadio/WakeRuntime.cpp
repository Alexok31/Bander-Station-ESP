#include "WakeRuntime.h"
#include "WakeFrontend.h"
#include "WakeDecision.h"
#include "WakeModelData.h"
#include "WakeVoiceSettings.h"
#include <Arduino.h>
#include <atomic>
#include <new>
#include <esp_heap_caps.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/schema/schema_generated.h>

namespace WakeRuntime {
namespace {
constexpr size_t arenaSize=256*1024, chunkSamples=240;
static_assert(WakeDecision::threshold == WakeModelData::threshold, "Do not silently change the model threshold");
struct Chunk { uint32_t epoch; int16_t pcm[chunkSamples]; };
static QueueHandle_t queue=nullptr;
static StaticQueue_t queueControl;
static uint8_t* queueStorage=nullptr;
static TaskHandle_t task=nullptr;
static std::atomic<bool> requested{false}, capturing{false};
static std::atomic<bool> voiceMode{false};
static std::atomic<uint8_t> followupTime{WakeVoiceSettings::defaultFollowupSeconds};
static uint32_t pendingEpoch=0, pendingMs=0; // Protected by statsMux.
static std::atomic<uint32_t> epoch{1};
static portMUX_TYPE statsMux=portMUX_INITIALIZER_UNLOCKED;
static Status stats{};
struct StatsGuard {
    StatsGuard(){portENTER_CRITICAL(&statsMux);}
    ~StatsGuard(){portEXIT_CRITICAL(&statsMux);}
};
void fail(uint8_t error) {
    requested.store(false); capturing.store(false); epoch.fetch_add(1);
    { StatsGuard guard; stats.error=error; }
    Serial.printf("[Wake] disabled error=%u\n",unsigned(error));
}
void worker(void*) {
    // Allocated once, only after explicit activation. No large internal heap
    // allocation and no interference with audio I2S0 or websocket ownership.
    auto* arena=(uint8_t*)heap_caps_aligned_alloc(16,arenaSize,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    auto* storage=heap_caps_malloc(sizeof(WakeFrontend::Stream),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    auto* frontend=storage ? new(storage) WakeFrontend::Stream : nullptr;
    tflite::MicroMutableOpResolver<5> resolver;
    const auto* model=tflite::GetModel(WakeModelData::model);
    uint8_t initError=0;
    if(!arena || !frontend) initError=1;
    else if(model->version()!=TFLITE_SCHEMA_VERSION ||
            resolver.AddConv2D()!=kTfLiteOk || resolver.AddDepthwiseConv2D()!=kTfLiteOk ||
            resolver.AddFullyConnected()!=kTfLiteOk || resolver.AddMean()!=kTfLiteOk ||
            resolver.AddLogistic()!=kTfLiteOk) initError=2;
    // Keep initialization failure terminal until reboot; avoid repeated heap
    // allocation on every mic chunk. The status tells the user what happened.
    if(initError) { fail(initError); free(arena); free(storage); vTaskSuspend(nullptr); }
    tflite::MicroInterpreter interpreter(model,resolver,arena,arenaSize);
    if(interpreter.AllocateTensors()!=kTfLiteOk) {fail(3);vTaskSuspend(nullptr);}
    auto* input=interpreter.input(0); auto* output=interpreter.output(0);
    if(!input || !output || input->type!=kTfLiteInt8 || output->type!=kTfLiteInt8 ||
       input->bytes!=WakeFrontend::inputSize || output->bytes!=1 ||
       input->dims->size!=4 || input->dims->data[1]!=238 || input->dims->data[2]!=40 ||
       input->dims->data[3]!=1 || input->params.zero_point!=WakeModelData::inputZero ||
       fabsf(input->params.scale-WakeModelData::inputScale)>1e-8f) {fail(3);vTaskSuspend(nullptr);}
    frontend->reset();
    { StatsGuard guard; stats.ready=true; stats.arenaBytes=interpreter.arena_used_bytes(); }
    Serial.printf("[Wake] ready model=v7 runtime=9 threshold=%.2f hop=%ums confirm=%u arena=%u heap=%u\n",
        WakeModelData::threshold,unsigned(WakeDecision::intervalMs),unsigned(WakeDecision::confirmations),
        unsigned(interpreter.arena_used_bytes()),unsigned(ESP.getFreeHeap()));
    WakeDecision::Trigger trigger;
    WakeDecision::Cadence cadence;
    uint32_t seen=0, accumulatedFrontendUs=0, lastLog=0;
    Chunk chunk;
    for(;;) {
        if(xQueueReceive(queue,&chunk,pdMS_TO_TICKS(100))!=pdTRUE) continue;
        if(!requested.load() || !capturing.load() || chunk.epoch!=epoch.load()) continue;
        if(seen!=chunk.epoch) {
            frontend->reset();trigger.reset();cadence.reset();seen=chunk.epoch;accumulatedFrontendUs=0;
            StatsGuard guard; ++stats.resets; stats.score=0;
        }
        uint32_t started=micros();
        for(const auto sample:chunk.pcm) {
            if(!frontend->push24(sample)) continue;
            if(!cadence.advance()) continue; // Preserve every sample/feature; skip only model invocations.
            frontend->quantize(input->data.int8,input->params.scale,input->params.zero_point);
            const uint32_t frontendUs=accumulatedFrontendUs+uint32_t(micros()-started);
            const uint32_t inferenceStarted=micros();
            if(interpreter.Invoke()!=kTfLiteOk) {fail(4);break;}
            const uint32_t inferenceUs=uint32_t(micros()-inferenceStarted);
            const uint32_t processing=frontendUs+inferenceUs;
            accumulatedFrontendUs=0;
            if(!requested.load() || !capturing.load() || seen!=epoch.load()) break;
            const float score=(int(output->data.int8[0])-output->params.zero_point)*output->params.scale;
            const bool hit=trigger.step(score);
            const uint32_t now=millis();
            {
                StatsGuard guard;
                ++stats.decisions; stats.score=score; stats.processingUs=processing;
                stats.frontendUs=frontendUs; stats.inferenceUs=inferenceUs;
                if(processing>=WakeDecision::intervalMs*1000) ++stats.overruns;
                const uint32_t queued=uxQueueMessagesWaiting(queue);
                if(queued>stats.queueHighWater) stats.queueHighWater=queued;
                if(processing>stats.peakProcessingUs) stats.peakProcessingUs=processing;
                stats.stackFreeBytes=uxTaskGetStackHighWaterMark(nullptr);
                if(hit) {
                    ++stats.detections;stats.lastHitMs=now;
                    if(voiceMode.load()) {pendingEpoch=seen;pendingMs=now;}
                }
            }
            if(hit) Serial.printf("[Wake] DETECT %s score=%.3f processing=%uus\n",voiceMode.load()?"voice":"diagnostic",score,unsigned(processing));
            if(uint32_t(now-lastLog)>=10000u) {
                lastLog=now;
                const auto s=status();
                Serial.printf("[Wake] score=%.3f hits=%u dropped=%u processing=%uus peak=%uus dsp=%uus nn=%uus hop=%ums overruns=%u qpeak=%u heap=%u psram=%u\n",
                    score,unsigned(s.detections),unsigned(s.dropped),unsigned(processing),unsigned(s.peakProcessingUs),
                    unsigned(frontendUs),unsigned(inferenceUs),unsigned(WakeDecision::intervalMs),unsigned(s.overruns),unsigned(s.queueHighWater),
                    unsigned(ESP.getFreeHeap()),unsigned(ESP.getFreePsram()));
            }
            vTaskDelay(1); // Let WiFi/core0 and idle tasks run even while catching up.
            started=micros();
        }
        accumulatedFrontendUs+=uint32_t(micros()-started);
    }
}
}
const char* enable(bool on, bool voice) {
    if(!on) { requested.store(false); voiceMode.store(false); listening(false); return nullptr; }
    if(status().error) return "Ошибка детектора. Перезапусти колонку и проверь журнал [Wake].";
    if(!queueStorage) queueStorage=(uint8_t*)heap_caps_malloc(24*sizeof(Chunk),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!queue && queueStorage) queue=xQueueCreateStatic(24,sizeof(Chunk),queueStorage,&queueControl);
    if(!queue) return "Недостаточно памяти для очереди микрофона.";
    epoch.fetch_add(1);voiceMode.store(voice);requested.store(true);
    if(!task && xTaskCreatePinnedToCore(worker,"wake_diag",8192,nullptr,1,&task,0)!=pdPASS) {
        requested.store(false); task=nullptr; return "Не удалось запустить детектор: недостаточно памяти.";
    }
    return nullptr;
}
bool enabled(){return requested.load();}
bool voiceEnabled(){return requested.load() && voiceMode.load();}
bool followupEnabled(){return voiceEnabled() && followupTime.load()!=0;}
uint8_t followupSeconds(){return followupTime.load();}
void setFollowupSeconds(uint8_t seconds){if(WakeVoiceSettings::validFollowup(seconds))followupTime.store(seconds);}
bool takeDetection() {
    StatsGuard guard;
    const bool valid=pendingEpoch && pendingEpoch==epoch.load() && capturing.load() &&
        voiceEnabled() && uint32_t(millis()-pendingMs)<1000u;
    pendingEpoch=0;
    return valid;
}
Status status() {
    StatsGuard guard; auto result=stats;
    result.enabled=requested.load(); result.listening=capturing.load(); return result;
}
void listening(bool active) {
    if(capturing.exchange(active)!=active) epoch.fetch_add(1);
}
void gap() {
    epoch.fetch_add(1); StatsGuard guard; ++stats.dropped;
}
void micError(){fail(5);}
void submit(const int16_t* pcm,size_t samples) {
    if(!queue || !requested.load() || !capturing.load()) return;
    if(samples!=chunkSamples) {gap();return;}
    Chunk chunk;chunk.epoch=epoch.load();memcpy(chunk.pcm,pcm,sizeof(chunk.pcm));
    if(xQueueSend(queue,&chunk,0)!=pdTRUE) gap();
}
}
