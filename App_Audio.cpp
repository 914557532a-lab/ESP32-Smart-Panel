/**
 * @file App_Audio.cpp
 * @brief 包含 RingBuffer 缓冲机制 (修复编译错误版)
 */
#include "App_Audio.h"
#include "Pin_Config.h" 
#include "AudioTools.h"
#include "AudioBoard.h"

AppAudio MyAudio;

static DriverPins my_pins;
static AudioBoard board(AudioDriverES8311, my_pins);
static I2SStream i2s; 

// ==========================================
//  前向声明 (解决 "was not declared" 错误)
// ==========================================
void audioPlayTask(void *param);
void playToneTaskWrapper(void *param);
void recordTaskWrapper(void *param);

struct ToneParams {
    int freq;
    int duration;
};

// ==========================================
//  AppAudio 类实现
// ==========================================

void AppAudio::init() {
    Serial.println("[Audio] Init...");
    pinMode(PIN_PA_EN, OUTPUT);
    digitalWrite(PIN_PA_EN, LOW); 

    my_pins.addI2C(PinFunction::CODEC, PIN_I2C_SCL, PIN_I2C_SDA, 0); 
    my_pins.addI2S(PinFunction::CODEC, PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DOUT, PIN_I2S_DIN);
    my_pins.addPin(PinFunction::PA, PIN_PA_EN, PinLogic::Output);

    CodecConfig cfg;
    cfg.input_device = ADC_INPUT_ALL; 
    cfg.output_device = DAC_OUTPUT_ALL;
    cfg.i2s.bits = BIT_LENGTH_16BITS;
    cfg.i2s.rate = RATE_16K; // 硬件 16k
    cfg.i2s.fmt = I2S_NORMAL; 

    if (board.begin(cfg)) Serial.println("[Audio] Codec OK");
    else Serial.println("[Audio] Codec FAIL");

    board.setVolume(25);      
    board.setInputVolume(0); 

    auto config = i2s.defaultConfig(RXTX_MODE); 
    config.pin_bck = PIN_I2S_BCLK;
    config.pin_ws = PIN_I2S_LRCK;
    config.pin_data = PIN_I2S_DOUT;
    config.pin_data_rx = PIN_I2S_DIN;
    config.pin_mck = PIN_I2S_MCLK; 
    config.sample_rate = AUDIO_SAMPLE_RATE; // 使用宏定义 16000
    config.bits_per_sample = 16;
    config.channels = 2;       
    config.use_apll = true;    
    
    if (i2s.begin(config)) Serial.println("[Audio] I2S OK");
    else Serial.println("[Audio] I2S FAIL");

    // 录音缓冲
    if (psramFound()) record_buffer = (uint8_t *)ps_malloc(MAX_RECORD_SIZE);
    else record_buffer = (uint8_t *)malloc(MAX_RECORD_SIZE);

    // [创建 RingBuffer]
    // 这里的 PLAY_BUFFER_SIZE 来自头文件，现在应该能找到了
    playRingBuf = xRingbufferCreate(PLAY_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (playRingBuf == NULL) {
        Serial.println("[Audio] RingBuffer Create Failed!");
    } else {
        Serial.println("[Audio] RingBuffer Created.");
    }

    // [启动播放任务]
    // 现在 audioPlayTask 已经在顶部声明过了，这里不会报错
    xTaskCreatePinnedToCore(audioPlayTask, "AudioPlay", 4096, this, 5, &playTaskHandle, 1);
    
    playToneAsync(1000, 200);
}

void AppAudio::setVolume(uint8_t vol) { board.setVolume(vol); }
void AppAudio::setMicGain(uint8_t gain) { board.setInputVolume(gain); }

void AppAudio::playToneAsync(int freq, int duration_ms) {
    ToneParams *params = (ToneParams*)malloc(sizeof(ToneParams));
    if(params) {
        params->freq = freq;
        params->duration = duration_ms;
        xTaskCreate(playToneTaskWrapper, "PlayTone", 4096, params, 2, NULL);
    }
}


void AppAudio::pushToPlayBuffer(uint8_t* data, size_t len) {
    if (playRingBuf == NULL || data == NULL || len == 0) return;
    
    // 循环尝试发送，直到成功
    // 如果缓冲区满了，network任务会在这里暂缓，自然就限制了下载速度（流量控制）
    while (xRingbufferSend(playRingBuf, data, len, pdMS_TO_TICKS(10)) != pdTRUE) {
        // 缓冲区满了，给播放任务一点时间去消耗数据
        vTaskDelay(1); 
    }
}

// [核心] 兼容旧接口 - 直接调用 push
void AppAudio::playChunk(uint8_t* data, size_t len) {
    pushToPlayBuffer(data, len);
}

// [核心] 流式播放 - 边下边推
void AppAudio::playStream(Client* client, int length) {
    if (!client || length <= 0) return;
    Serial.printf("[Audio] Stream Push: %d bytes\n", length);
    const int buff_size = 1024; 
    uint8_t buff[buff_size]; 
    int remaining = length;
    while (remaining > 0 && client->connected()) {
        int max_read = (remaining > buff_size) ? buff_size : remaining;
        int bytesIn = 0;
        unsigned long startWait = millis();
        while (bytesIn < max_read && millis() - startWait < 2000) {
             if (client->available()) {
                 int r = client->read(buff + bytesIn, max_read - bytesIn);
                 if (r > 0) bytesIn += r;
             } else delay(1);
        }
        if (bytesIn == 0) break; 
        pushToPlayBuffer(buff, bytesIn);
        remaining -= bytesIn;
    }
}

void AppAudio::startRecording() {
    if (isRecording) return;
    if (!record_buffer) return;
    board.setInputVolume(85); 
    record_data_len = 44; 
    isRecording = true;
    xTaskCreate(recordTaskWrapper, "RecTask", 8192, this, 10, &recordTaskHandle);
    Serial.println("[Audio] Start Rec");
}

void AppAudio::stopRecording() {
    isRecording = false; 
    delay(100); 
    board.setInputVolume(0); 
    uint32_t pcm_size = record_data_len - 44;
    createWavHeader(record_buffer, pcm_size, AUDIO_SAMPLE_RATE, 16, 2);
}

void AppAudio::_recordTask(void *param) {
    const size_t read_size = 1024; 
    uint8_t temp_buf[read_size]; 
    while (isRecording) {
        size_t bytes_read = i2s.readBytes(temp_buf, read_size);
        if (bytes_read > 0) {
            if (record_data_len + bytes_read < MAX_RECORD_SIZE) {
                memcpy(record_buffer + record_data_len, temp_buf, bytes_read);
                record_data_len += bytes_read;
            } else isRecording = false; 
        } else vTaskDelay(1);
    }
}

void AppAudio::createWavHeader(uint8_t *header, uint32_t totalDataLen, uint32_t sampleRate, uint8_t sampleBits, uint8_t numChannels) {
    uint32_t byteRate = sampleRate * numChannels * (sampleBits / 8);
    uint32_t totalFileSize = totalDataLen + 44 - 8;
    // (WAV头生成代码保持原样，省略以节省篇幅，功能不变)
    // ... 请保持原有的 WAV Header 生成逻辑 ...
    header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
    header[4] = (uint8_t)(totalFileSize & 0xFF);
    header[5] = (uint8_t)((totalFileSize >> 8) & 0xFF);
    header[6] = (uint8_t)((totalFileSize >> 16) & 0xFF);
    header[7] = (uint8_t)((totalFileSize >> 24) & 0xFF);
    header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
    header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
    header[20] = 1; header[21] = 0; 
    header[22] = numChannels; header[23] = 0; 
    header[24] = (uint8_t)(sampleRate & 0xFF);
    header[25] = (uint8_t)((sampleRate >> 8) & 0xFF);
    header[26] = (uint8_t)((sampleRate >> 16) & 0xFF);
    header[27] = (uint8_t)((sampleRate >> 24) & 0xFF);
    header[28] = (uint8_t)(byteRate & 0xFF);
    header[29] = (uint8_t)((byteRate >> 8) & 0xFF);
    header[30] = (uint8_t)((byteRate >> 16) & 0xFF);
    header[31] = (uint8_t)((byteRate >> 24) & 0xFF);
    header[32] = (uint8_t)(numChannels * (sampleBits / 8)); header[33] = 0; 
    header[34] = sampleBits; header[35] = 0;
    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    header[40] = (uint8_t)(totalDataLen & 0xFF);
    header[41] = (uint8_t)((totalDataLen >> 8) & 0xFF);
    header[42] = (uint8_t)((totalDataLen >> 16) & 0xFF);
    header[43] = (uint8_t)((totalDataLen >> 24) & 0xFF);
}

// ==========================================
//  后台任务具体实现
// ==========================================

void audioPlayTask(void *param) {
    AppAudio *audio = (AppAudio *)param;
    size_t item_size;
    unsigned long last_audio_time = millis();
    bool pa_enabled = false;
    const int PA_TIMEOUT_MS = 2000;

    while (1) {
        // [修复] 这里访问 audio->playRingBuf 没问题，因为现在它是 public 的
        uint8_t *item = (uint8_t *)xRingbufferReceive(audio->playRingBuf, &item_size, pdMS_TO_TICKS(100));
        
        if (item != NULL) {
            if (!pa_enabled) {
                digitalWrite(PIN_PA_EN, HIGH);
                pa_enabled = true;
            }
            last_audio_time = millis();

            // Mono -> Stereo
            size_t samples = item_size / 2;
            int16_t *pcm_in = (int16_t *)item;
            int16_t stereo_batch[256]; 
            size_t processed = 0;
            
            while(processed < samples) {
                size_t chunk = (samples - processed) > 128 ? 128 : (samples - processed);
                for(int i=0; i<chunk; i++) {
                     int16_t val = pcm_in[processed + i];
                     stereo_batch[i*2] = val;
                     stereo_batch[i*2+1] = val;
                }
                i2s.write((uint8_t*)stereo_batch, chunk * 4);
                processed += chunk;
            }
            vRingbufferReturnItem(audio->playRingBuf, (void *)item);
        } else {
            if (pa_enabled && (millis() - last_audio_time > PA_TIMEOUT_MS)) {
                digitalWrite(PIN_PA_EN, LOW);
                pa_enabled = false;
            }
        }
    }
}

void playToneTaskWrapper(void *param) {
    ToneParams *p = (ToneParams*)param;
    digitalWrite(PIN_PA_EN, HIGH);
    const int sample_rate = AUDIO_SAMPLE_RATE;
    const int amplitude = 10000; 
    int total_samples = (sample_rate * p->duration) / 1000;
    int16_t sample_buffer[256]; 
    for (int i = 0; i < total_samples; i += 128) {
        int batch = (total_samples - i) > 128 ? 128 : (total_samples - i);
        for (int j = 0; j < batch; j++) {
            int16_t val = (int16_t)(amplitude * sin(2 * PI * p->freq * (i + j) / sample_rate));
            sample_buffer[2*j] = val;     
            sample_buffer[2*j+1] = val;   
        }
        i2s.write((uint8_t*)sample_buffer, batch * 4);
        if(i % 1024 == 0) delay(1);
    }
    // digitalWrite(PIN_PA_EN, LOW); // 交给 audioPlayTask 自动关
    free(p);
    vTaskDelete(NULL); 
}

void recordTaskWrapper(void *param) {
    AppAudio *audio = (AppAudio *)param;
    audio->_recordTask(NULL); 
    vTaskDelete(NULL);
}

// C 接口
void Audio_Play_Click() { MyAudio.playToneAsync(1000, 100); }
void Audio_Record_Start() { MyAudio.startRecording(); }
void Audio_Record_Stop() { MyAudio.stopRecording(); }