/*********************************************************************
 * PIZZA COOKER OS v4.4
 * ---------------------------------------------------------------
 * [主要機能]
 * ・二重化熱電対によるヒーター/プレートの個別温度監視
 * ・PID制御およびオートチューニング機能
 * ・電力制限枠内での動的PWM配分（下火優先アルゴリズム）
 * ・ストーン熱浸透度（Soak）の計算と自動焼き開始判定
 *********************************************************************/

#include <Arduino.h>
#include <EEPROM.h>
#include <max6675.h>
#include <PID_AutoTune_v0.h>
#include <U8x8lib.h>
#include <avr/wdt.h>

// Arduino標準のabs(float)マクロは意図しない型変換を起こす可能性があるため、明示的なfloat版を定義
static inline float f_abs(float v) { return (v < 0.0f) ? -v : v; }

/* ================= CONFIGURATION ================= */
namespace Config {
    // EEPROMのデータ構造が変わった際に初期化を強制するための識別子
    constexpr uint32_t EEPROM_MAGIC = 0x50495A36; 

    namespace Pins {
        constexpr uint8_t THERMO_CLK   = 15, THERMO_DO = 14;
        constexpr uint8_t CS_UP_PLATE  = A1, CS_UP_HEATER  = A0; // 上火（プレート/ヒーター）
        constexpr uint8_t CS_LO_PLATE  = A3, CS_LO_HEATER  = A2; // 下火（プレート/ヒーター）
        constexpr uint8_t SSR_UP       = 5,   SSR_LO       = 6;  // PWM制御用SSR
        constexpr uint8_t SAFETY_RELAY = 9;                      // 主電源遮断用リレー
        constexpr uint8_t ENC_CLK      = 7,   ENC_DT       = 8;  // ロータリーエンコーダ
        constexpr uint8_t ENC_SW       = 4;                      // エンコーダプッシュスイッチ
    }

    namespace Hard {
        constexpr float RATED_UP_W        = 850.0f; // 上ヒーター定格
        constexpr float RATED_LO_W        = 570.0f; // 下ヒーター定格
        constexpr float STONE_THICK_MM    = 4.0f;   // ストーン厚（Soak計算に使用）
        constexpr float PLATE_MAX_C       = 650.0f; // 安全限界温度
        constexpr float HEATER_MAX_C      = 820.0f; // ヒーター損傷限界温度
        constexpr float COOL_COMPLETE_C   = 100.0f; // 冷却完了判定温度
        constexpr uint32_t RUNAWAY_TIMEOUT_MS   = 30000UL; // 暴走判定（出力0で温度上昇時）
        constexpr uint32_t REST_TIMEOUT_MS      = 30UL * 60UL * 1000UL; // 無操作自動停止
        constexpr uint32_t EEPROM_IDLE_TIMEOUT_MS = 30000UL; // 書き込み待機時間
        constexpr uint32_t BOOST_MS             = 30000UL;    // 投入直後の電力ブースト時間
        constexpr uint32_t BAKE_DONE_MSG_MS     = 3000UL;    // 完了メッセージ表示時間
        constexpr float    TUNE_TARGET_C        = 350.0f;    // オートチューニング目標
    }

    namespace Msg {
        const char PREHEAT[] PROGMEM   = "Soaking...";
        const char REST[] PROGMEM      = "I'll be back";
        const char COOL[] PROGMEM      = "I'll be cool";
        const char DONE[] PROGMEM      = "Well done. Ciao!";
        const char ERROR[] PROGMEM     = "Safety Stop";
        const char BAKE_DONE[] PROGMEM = "Buon appetito!";
    }

#pragma pack(push, 1) // メモリ節約のためパディングを禁止
    struct Recipe {
        char name[8];
        float upC, loC;      // 目標温度
        char readyMsg[22];   // 到達時メッセージ
        uint16_t bakeSec;    // 標準焼き時間
    };
#pragma pack(pop)

    const Recipe recipes[] PROGMEM = {
        {"Napoli", 500.0f, 430.0f, "Pizza Time",     90},
        {"Romana", 330.0f, 310.0f, "Crispy Romana", 180}
    };
    constexpr uint8_t RECIPE_CNT = sizeof(recipes) / sizeof(Recipe);

    struct Limit { char label[6]; float watts; };
    const Limit limits[] PROGMEM = {
        {"1.4kW", 1420.0f}, // 15Aコンセント許容範囲
        {"1.0kW", 1000.0f},
        {"0.7kW",  700.0f}
    };
    constexpr uint8_t LIMIT_CNT = sizeof(limits) / sizeof(Limit);
}

namespace AskConfirmation {
    constexpr uint8_t NONE = 0;
    constexpr uint8_t CANCEL_TUNE = 1;
    constexpr uint8_t START_TUNE = 2;
    constexpr uint8_t FACTORY_RESET = 3;
}

/* ================= HEATER CONTROL CLASS ================= */
// 1つのヒーターユニット（プレート+ヒーターの2個のセンサー）を管理するクラス
class IntelligentHeater {
public:
    float plateC = 0, heaterC = 0, soak = 0, trend = 0;
    uint8_t pwm = 0, error = 0; // error bit: 0:Sensor, 1:Runaway, 2:Overheat

    IntelligentHeater(uint8_t csP, uint8_t csH, uint8_t ssr)
        : _plate(Config::Pins::THERMO_CLK, csP, Config::Pins::THERMO_DO),
          _heater(Config::Pins::THERMO_CLK, csH, Config::Pins::THERMO_DO),
          _ssr(ssr) {
        pinMode(_ssr, OUTPUT);
        digitalWrite(_ssr, LOW);
        _winStart = millis();
    }

    // 制御サイクルの実行（毎秒呼び出し）
    // インライン展開を防ぎFlashを節約
    bool tick(float target, float &health) __attribute__((noinline)) {
        float rp = _plate.readCelsius(), rh = _heater.readCelsius();

        // [異常検知] センサーエラー時は即座にPWMを0にし、SSRを物理的に停止
        if (isnan(rp) || rp < 0.0f || rp > Config::Hard::PLATE_MAX_C || // プレートセンサーの異常値
            isnan(rh) || rh < 0.0f || rh > Config::Hard::HEATER_MAX_C + 100.0f) { // ヒーターセンサーの異常値
            error |= 1; pwm = 0; _out = 0; digitalWrite(_ssr, LOW); return false;
        }
        error &= ~1;
        heaterC = rh;

        // 初回起動時の温度追従
        if (_first) { 
            plateC = rp; _first = false; _runawayMs = millis(); 
            _lastInput = plateC;
        }

        // [フィルタリング] 温度変化を平滑化
        float prev = plateC;
        plateC = 0.8f * plateC + 0.2f * rp;
        trend = 0.9f * trend + 0.1f * (plateC - prev); // 温度勾配（トレンド）を算出

        // [Soak計算] ストーンの芯まで熱が通ったかをシミュレート
        float step = 1.0f / Config::Hard::STONE_THICK_MM;
        if (target > 50.0f && f_abs(target - plateC) < 5.0f)
            soak = min(100.0f, soak + step); // 目標付近なら浸透
        else
            soak = max(0.0f, soak - step * 0.5f); // 離れていれば放熱

        _in  = plateC;
        _set = target;

        // PID演算またはオートチューニングの実行
        if (_tuning) {
            if (_aTune && _aTune->Runtime() != 0) {
                setTunings(_aTune->GetKp(), _aTune->GetKi(), _aTune->GetKd());
                stopTune(); // チューニング完了処理（メモリ解放含む）
            }
        } else {
            // 簡易PID計算 (float統一でコードサイズ削減)
            float error = _set - _in;
            _iTerm += (_ki * error);
            if (_iTerm > 255.0f) _iTerm = 255.0f; else if (_iTerm < 0.0f) _iTerm = 0.0f;
            
            float dInput = (_in - _lastInput);
            float output = _kp * error + _iTerm - _kd * dInput;
            
            if (output > 255.0f) output = 255.0f; else if (output < 0.0f) output = 0.0f;
            _out = output;
            _lastInput = _in;
        }
        pwm = static_cast<uint8_t>(_out);

        // [暴走検知] 出力0なのに急激に温度が上がっている場合はSSRの短絡を疑う
        uint32_t now = millis();
        if (pwm == 0 && trend > 1.5f) {
            if (now - _runawayMs > Config::Hard::RUNAWAY_TIMEOUT_MS) error |= 2;
        } else {
            _runawayMs = now;
        }

        // [健康度/過昇温管理] ヒーターフィラメントのダメージを累積計算
        float limit = Config::Hard::HEATER_MAX_C;
        bool damaged = false;
        if (heaterC > limit + 40.0f) {
            health = max(0.0f, health - 0.01f);
            damaged = true;
        } else if (heaterC > limit + 20.0f) {
            health = max(0.0f, health - 0.002f);
            damaged = true;
        }

        if (plateC > Config::Hard::PLATE_MAX_C) error |= 4;
        return damaged;
    }

    // SSRのタイムプロポーショニング制御（1秒周期内でのON時間を制御）
    void drive(uint8_t out) {
        if (out != _lastOut) {
            _onTimeMs = static_cast<uint32_t>(out) * 1000 / 255;
            _lastOut = out;
        }
        uint32_t now = millis();
        if (now - _winStart >= 1000UL) _winStart = now;
        digitalWrite(_ssr, (now - _winStart < _onTimeMs) ? HIGH : LOW);
    }

    // エラーや状態遷移時のリセット処理
    void reset() { 
        error = 0; pwm = 0; _out = 0; soak = 0; trend = 0; _overheatCnt = 0;
        _first = true; digitalWrite(_ssr, LOW);
        plateC = 0; heaterC = 0;
        _runawayMs = millis(); _winStart = millis();
        _iTerm = 0; _lastInput = 0; // PID内部変数のリセット
    }
    
    float pidOut() const { return _out; }
    void setTunings(float kp, float ki, float kd) { _kp = kp; _ki = ki; _kd = kd; }
    float getKp() { return _kp; }
    float getKi() { return _ki; }
    float getKd() { return _kd; }

    void startTune() {
        if (_aTune) delete _aTune; // 念のため既存があれば削除
        _aTune = new PID_ATune((double*)&_in, (double*)&_out);
        _tuning = true;
        _aTune->SetNoiseBand(2);
        _aTune->SetOutputStep(255);
        _aTune->SetLookbackSec(12);
        _aTune->SetControlType(1);
    }
    void stopTune() { _tuning = false; if (_aTune) { delete _aTune; _aTune = nullptr; } }
    bool isTuning() const { return _tuning; }

private:
    MAX6675 _plate, _heater;
    PID_ATune* _aTune = nullptr; // メモリ節約のためポインタに戻す
    uint8_t _ssr;
    float  _in, _out, _set;
    uint32_t _runawayMs = 0, _winStart = 0;
    bool     _first = true, _tuning = false;
    uint8_t _overheatCnt = 0;
    uint16_t _lastOut = 256; // キャッシュ用（初期値は範囲外）
    uint32_t _onTimeMs = 0;
    float _kp = 3.5f, _ki = 0.05f, _kd = 1.0f;
    float _iTerm = 0.0f, _lastInput = 0.0f;
};

/* ================= GLOBALS ================= */
IntelligentHeater up(Config::Pins::CS_UP_PLATE, Config::Pins::CS_UP_HEATER, Config::Pins::SSR_UP);
IntelligentHeater lo(Config::Pins::CS_LO_PLATE, Config::Pins::CS_LO_HEATER, Config::Pins::SSR_LO);
// U8x8モード（バッファレス・高速・省メモリ）で初期化
U8X8_SH1106_128X64_NONAME_HW_I2C oled(/* reset=*/ U8X8_PIN_NONE);

enum class OvenState : uint8_t { IDLE, PREHEAT, READY, BAKING, BAKE_DONE, REST, COOLING, SHUTDOWN, ERROR, TUNING };
OvenState oven = OvenState::IDLE, prevOven = OvenState::IDLE;

#pragma pack(push, 1)
struct Settings { 
    uint32_t magic; uint8_t recipeIdx, limitIdx; float upHealth, loHealth; 
    float upKp, upKi, upKd;
    float loKp, loKi, loKd;
} settings;
Settings lastSaveSettings; // EEPROMに保存されている値のシャドウコピー
#pragma pack(pop)

bool baking = false;
uint8_t askConfirmation = AskConfirmation::NONE; // 現在表示中の確認プロンプトID
bool confirmationYes = false;              // プロンプトでの選択状態 (Y/N)
uint8_t tuneStage = 0;                     // オートチューニングの進行状況
uint16_t curBakeSec = 0;
uint32_t bakeStartMs = 0, bakeDoneMsgMs = 0, boostStartMs = 0, restStartMs = 0, lastActMs = 0;
float lastSavedUpHealth = 100.0f;
float lastSavedLoHealth = 100.0f;
Config::Recipe currentRecipe; // 現在のレシピを保持するキャッシュ
uint8_t targetUpPWM = 0, targetLoPWM = 0; // 計算済みのPWM値

const __FlashStringHelper* temporaryMsg = nullptr;
uint32_t temporaryMsgEndMs = 0;

// EEPROMへの遅延書き込み処理（頻繁な書き込みによる寿命低下を防止）
void dirtySave(bool set = false) {
    static bool dirty = false; static uint32_t lastMs = 0;
    if (set) { dirty = true; lastMs = millis(); return; }
    
    // タイムアウト経過、または即時保存が必要なステート（SHUTDOWN/ERROR）の場合
    bool timeout = (millis() - lastMs > Config::Hard::EEPROM_IDLE_TIMEOUT_MS);
    bool urgent = (oven == OvenState::SHUTDOWN || oven == OvenState::ERROR);

    if (dirty && (timeout || urgent)) {
        dirty = false;
        // 実際に値が変更されている場合のみ書き込み（RAM上のシャドウコピーと比較）
        if (memcmp(&settings, &lastSaveSettings, sizeof(Settings)) != 0) {
            EEPROM.put(0, settings);
            lastSaveSettings = settings;
        }
    }
}

void startBake(uint16_t sec) {
    baking = true; curBakeSec = sec;
    bakeStartMs = boostStartMs = lastActMs = millis();
    oven = OvenState::BAKING;
}

// エンコーダとスイッチのデバウンス処理付き入力管理
void handleInput(uint32_t now) {
    static int lastClk = HIGH; 
    static uint32_t lastEncMs = 0;
    int clk = digitalRead(Config::Pins::ENC_CLK);

    if (lastClk == HIGH && clk == LOW && now - lastEncMs > 50UL) {
        lastEncMs = now;
        lastActMs = now;
        int dir = (digitalRead(Config::Pins::ENC_DT) != LOW) ? 1 : -1;
        if (askConfirmation != AskConfirmation::NONE) {
            confirmationYes = !confirmationYes; // Y/N 切り替え
        } else if (oven != OvenState::ERROR && oven != OvenState::TUNING) {
            settings.recipeIdx = (settings.recipeIdx + dir + Config::RECIPE_CNT) % Config::RECIPE_CNT;
            memcpy_P(&currentRecipe, &Config::recipes[settings.recipeIdx], sizeof(currentRecipe));
            dirtySave(true);
        }
    }
    lastClk = clk;

    static bool lastSw = HIGH;
    static uint32_t pressStartMs = 0;
    static bool longPressHandled = false;
    const uint32_t LONG_PRESS_MS = 2000UL;
    const uint32_t DEBOUNCE_MS = 50UL;

    bool sw = digitalRead(Config::Pins::ENC_SW);

    if (sw == LOW && lastSw == HIGH) { // Button just pressed
        pressStartMs = now;
        longPressHandled = false;
    } else if (sw == HIGH && lastSw == LOW) { // Button just released
        if (now - pressStartMs > DEBOUNCE_MS && !longPressHandled) {
            // Short press action
            lastActMs = now;
            if (askConfirmation != AskConfirmation::NONE) {
                if (confirmationYes) {
                    // [汎用] 選択されたアクションの実行
                    if (askConfirmation == AskConfirmation::CANCEL_TUNE) {
                        up.stopTune(); lo.stopTune();
                        up.reset(); lo.reset();
                        oven = OvenState::SHUTDOWN;
                        tuneStage = 0; // 進行状況をリセット
                        temporaryMsg = F("Canceled");
                        temporaryMsgEndMs = now + 2000UL;
                        dirtySave(true);
                    } else if (askConfirmation == AskConfirmation::START_TUNE) {
                        up.reset(); lo.reset();
                        oven = OvenState::TUNING;
                        tuneStage = 0;
                        temporaryMsg = F("Tuning Start");
                        temporaryMsgEndMs = now + 2000UL;
                    } else if (askConfirmation == AskConfirmation::FACTORY_RESET) {
                        // デフォルト値の設定と保存
                        settings = {
                            Config::EEPROM_MAGIC, 0, 0, 100.0f, 100.0f, 
                            3.5f, 0.05f, 1.0f, 
                            3.5f, 0.05f, 1.0f
                        };
                        EEPROM.put(0, settings);
                        lastSaveSettings = settings;
                        // 設定を即時反映
                        up.setTunings(settings.upKp, settings.upKi, settings.upKd);
                        lo.setTunings(settings.loKp, settings.loKi, settings.loKd);
                        memcpy_P(&currentRecipe, &Config::recipes[settings.recipeIdx], sizeof(currentRecipe));
                        
                        up.reset(); lo.reset();
                        oven = OvenState::SHUTDOWN;
                        
                        temporaryMsg = F("Factory Reset");
                        temporaryMsgEndMs = now + 2000UL;
                    }
                }
                askConfirmation = AskConfirmation::NONE; // プロンプトを閉じる
            } else if (oven != OvenState::ERROR && oven != OvenState::TUNING) {
                settings.limitIdx = (settings.limitIdx + 1) % Config::LIMIT_CNT;
                dirtySave(true);
            }
        }
    } else if (sw == LOW && !longPressHandled) { // Button is being held down
        if (now - pressStartMs > LONG_PRESS_MS) {
            // Long press action
            lastActMs = now;
            if (oven == OvenState::TUNING) {
                askConfirmation = AskConfirmation::CANCEL_TUNE; // キャンセルメニュー表示
                confirmationYes = false; // デフォルトはNo
                longPressHandled = true;
            } else if (oven == OvenState::ERROR) {
                oven = OvenState::IDLE;
                digitalWrite(Config::Pins::SAFETY_RELAY, HIGH);
                temporaryMsg = F("System Reset");
                temporaryMsgEndMs = now + 1000UL;
            } else if (oven == OvenState::IDLE) {
                askConfirmation = AskConfirmation::FACTORY_RESET;
                confirmationYes = false;
            }
            longPressHandled = true;
        }
    }
    lastSw = sw;
}

// 全体電力を制限枠内に収めるための動的PWM制限アルゴリズム
void calculatePower(uint32_t now) {
    if (oven == OvenState::TUNING) {
        targetUpPWM = static_cast<uint8_t>(up.pidOut());
        targetLoPWM = static_cast<uint8_t>(lo.pidOut());
        return;
    }
    
    // 浮動小数点演算を回避し、整数演算(int32_t)で処理することで高速化
    Config::Limit lim;
    memcpy_P(&lim, &Config::limits[settings.limitIdx], sizeof(lim));
    int32_t limW = static_cast<int32_t>(lim.watts);
    int32_t ratedUp = static_cast<int32_t>(Config::Hard::RATED_UP_W);
    int32_t ratedLo = static_cast<int32_t>(Config::Hard::RATED_LO_W);

    // Boostモード: 生地の投入による下火の低下を防ぐため、下火に優先的に電力を割り当てる
    if (baking && now - boostStartMs < Config::Hard::BOOST_MS) {
        int32_t upReqW = (static_cast<int32_t>(up.pidOut()) * ratedUp) / 255;
        int32_t loReqW = (static_cast<int32_t>(lo.pidOut()) * ratedLo) / 255;

        int32_t loActiveW = (limW < loReqW) ? limW : loReqW; 
        int32_t upMaxW = (limW - loActiveW > 0) ? (limW - loActiveW) : 0;

        int32_t upAllocW = (upReqW < upMaxW) ? upReqW : upMaxW;
        targetUpPWM = static_cast<uint8_t>((upAllocW * 255) / ratedUp);
        targetLoPWM = static_cast<uint8_t>((loActiveW * 255) / ratedLo);
    } else {
        // 通常時: 下火を最優先し、残りの電力枠を上火に提供
        int32_t loReqW = (static_cast<int32_t>(lo.pidOut()) * ratedLo) / 255;
        int32_t loW = (loReqW < limW) ? loReqW : limW;
        
        int32_t remW = (limW - loW > 0) ? (limW - loW) : 0;
        int32_t upReqW = (static_cast<int32_t>(up.pidOut()) * ratedUp) / 255;
        int32_t upW = (upReqW < remW) ? upReqW : remW;

        targetUpPWM = static_cast<uint8_t>((upW * 255) / ratedUp);
        targetLoPWM = static_cast<uint8_t>((loW * 255) / ratedLo);
    }
    
    // 重大なエラーが発生している場合は出力を強制遮断
    if (up.error || lo.error || oven == OvenState::ERROR) { targetUpPWM = targetLoPWM = 0; }
}

void renderOLED() {
    // oled.clear(); // 削除：点滅防止のため

    oled.setFont(u8x8_font_chroma48medium8_r); 

    // 1行目：レシピ名と電力制限 (上書き用に空白を付加)
    oled.setCursor(0, 0); oled.print(currentRecipe.name);
    oled.print(F("        ")); // 古い名前を消すための空白
    oled.setCursor(11, 0);
    char limitLabel[6];
    strcpy_P(limitLabel, Config::limits[settings.limitIdx].label);
    oled.print(limitLabel);
    
    // 2-3行目：温度 (2x2倍角)
    // Uは0列、Lは8列から開始。2x2なので行2と行3を占有します。
    oled.setFont(u8x8_font_px437wyse700b_2x2_r);
    oled.setCursor(0, 2); oled.print(F("U")); oled.print((int)up.plateC);
    oled.print(F(" ")); // 桁数が減った時のゴミ消し
    
    oled.setCursor(8, 2); oled.print(F("L")); oled.print((int)lo.plateC);
    oled.print(F(" "));
    
    oled.setFont(u8x8_font_chroma48medium8_r); 

    // 4行目：Soak と 警告
    // 警告は温度のすぐ下（行4）に配置
    oled.setCursor(0, 4);
    if (settings.upHealth < 20.0f || settings.loHealth < 20.0f) {
        oled.print(F("!! MAINT !! ")); 
    } else {
        oled.print(F("            ")); // 警告が消えた時にクリア
    }
    
    oled.setCursor(12, 4); 
    int sk = (int)min(up.soak, lo.soak);
    if(sk < 100) oled.print(F(" ")); // 桁揃え
    oled.print(sk); oled.print(F("%"));

    // 5-6行目：焼き時間
    oled.setCursor(0, 5);
    if (oven == OvenState::BAKING) {
        int32_t rem = static_cast<int32_t>(curBakeSec) - static_cast<int32_t>((millis() - bakeStartMs) / 1000);
        oled.print(F("Bake: ")); oled.print(max(0L, rem)); oled.print(F("s  "));
    } else {
        oled.print(F("                ")); // 非表示時にクリア
    }
    
    // 7行目：ステータス (最下段)
    oled.setCursor(0, 7);
    char buf[17];
    const char* src = nullptr;
    bool isProgmem = false;
    
    if (temporaryMsg != nullptr && millis() < temporaryMsgEndMs) {
        src = (const char*)temporaryMsg; isProgmem = true;
    } else if (askConfirmation != AskConfirmation::NONE) {
        // [汎用] プロンプト表示の構築
        const char* title = (askConfirmation == AskConfirmation::CANCEL_TUNE) ? "Cancel?" : 
                            (askConfirmation == AskConfirmation::START_TUNE) ? "Tune?" : 
                            (askConfirmation == AskConfirmation::FACTORY_RESET) ? "Reset?" : "Sure?";
        strcpy(buf, title); strcat(buf, confirmationYes ? " [Y] N" : " Y [N]");
    } else {
        temporaryMsg = nullptr;
        switch (oven) {
            case OvenState::PREHEAT:   src = Config::Msg::PREHEAT; isProgmem = true; break;
            case OvenState::READY:     src = currentRecipe.readyMsg; isProgmem = false; break;
            case OvenState::BAKING:    src = "Baking..."; isProgmem = false; break;
            case OvenState::BAKE_DONE: src = Config::Msg::BAKE_DONE; isProgmem = true; break;
            case OvenState::REST:      src = Config::Msg::REST; isProgmem = true; break;
            case OvenState::COOLING:   src = Config::Msg::COOL; isProgmem = true; break;
            case OvenState::ERROR:     src = Config::Msg::ERROR; isProgmem = true; break;
            case OvenState::TUNING:    src = "Auto Tuning..."; isProgmem = false; break;
            default: break;
        }
    }
    
    // バッファへのコピーとパディングを1パスで実行（memset/strncpyのオーバーヘッド削減）
    size_t i = 0;
    if (src) {
        while (i < 16) {
            char c = isProgmem ? pgm_read_byte(src + i) : src[i];
            if (c == '\0') break;
            buf[i++] = c;
        }
    } else if (askConfirmation != AskConfirmation::NONE) {
        i = strlen(buf); // bufは既に埋まっている
    }
    while (i < 16) buf[i++] = ' ';
    buf[16] = '\0';
    oled.print(buf);
}

void updateDisplay(uint32_t now) {
    static uint32_t lastOledMs = 0;
    // Flash節約のため、部分更新ロジックを廃止し、単純な定期更新に戻す
    // 1秒経過、またはオーブンの状態（IDLE/BAKING等）が変わった時のみ再描画
    if (oven != prevOven || now - lastOledMs >= 1000UL) {
        renderOLED();
        prevOven = oven;
        lastOledMs = now;
    }
}

// メインのステートマシンおよび制御ロジック
void runControlTick(uint32_t now) {
    static uint32_t lastCtrlMs = 0;
    if (now - lastCtrlMs >= 1000UL) {
        lastCtrlMs = now;
        const Config::Recipe &r = currentRecipe;

        // [TUNINGステート] PIDパラメーターの自動計測
        if (oven == OvenState::TUNING) {
            if (tuneStage == 0) {
                up.startTune(); tuneStage = 1;
            } else if (tuneStage == 1 && !up.isTuning()) {
                settings.upKp = up.getKp(); settings.upKi = up.getKi(); settings.upKd = up.getKd();
                dirtySave(true); tuneStage = 2;
            } else if (tuneStage == 2) {
                lo.startTune(); tuneStage = 3;
            } else if (tuneStage == 3 && !lo.isTuning()) {
                settings.loKp = lo.getKp(); settings.loKi = lo.getKi(); settings.loKd = lo.getKd();
                dirtySave(true); oven = OvenState::SHUTDOWN; tuneStage = 0;
            }
            if (tuneStage == 1) {
                up.tick(Config::Hard::TUNE_TARGET_C, settings.upHealth);
                lo.tick(0, settings.loHealth);
            }
            if (tuneStage == 3) {
                lo.tick(Config::Hard::TUNE_TARGET_C, settings.loHealth);
                up.tick(0, settings.upHealth);
            }

            // チューニング中も安全装置は常に監視する
            if (up.error || lo.error) {
                oven = OvenState::ERROR;
                up.stopTune(); lo.stopTune();
                up.reset(); lo.reset();
                digitalWrite(Config::Pins::SAFETY_RELAY, LOW);
                dirtySave(true);
            }
            return;
        }

        if (oven == OvenState::IDLE && askConfirmation == AskConfirmation::NONE) {
            oven = OvenState::PREHEAT;
            up.reset(); lo.reset();
        }

        // ヒーターを稼働させるステートの判定
        bool isHeating = (oven != OvenState::REST && oven != OvenState::COOLING && 
                          oven != OvenState::SHUTDOWN && oven != OvenState::ERROR
                          && askConfirmation == AskConfirmation::NONE);
        
        bool hUp = up.tick(isHeating ? r.upC : 0, settings.upHealth);
        bool hLo = lo.tick(isHeating ? r.loC : 0, settings.loHealth);

        // 健康度の保存処理
        if (hUp || hLo) {
            if (f_abs(settings.upHealth - lastSavedUpHealth) >= 1.0f || f_abs(settings.loHealth - lastSavedLoHealth) >= 1.0f) {
                dirtySave(true);
                lastSavedUpHealth = settings.upHealth; lastSavedLoHealth = settings.loHealth;
            }
        }

        // [READY判定] 温度誤差5度以内、かつ熱浸透度(Soak)が95%以上
        bool ready = (f_abs(up.plateC - r.upC) < 5.0f && f_abs(lo.plateC - r.loC) < 5.0f && min(up.soak, lo.soak) > 95.0f);

        // [BAKE判定] READY状態でピザを投入（下火温度の急下降）した際に自動開始
        if (!baking && (oven == OvenState::PREHEAT || oven == OvenState::READY)) {
            oven = ready ? OvenState::READY : OvenState::PREHEAT;
            if (ready && lo.trend < -2.0f) startBake(r.bakeSec);
            if (now - lastActMs > Config::Hard::REST_TIMEOUT_MS) { 
                oven = OvenState::REST; restStartMs = now; }
        }

        // 焼き上がり・メッセージ表示時間の管理
        if (baking && (now - bakeStartMs >= curBakeSec * 1000UL)) { 
            baking = false; oven = OvenState::BAKE_DONE; bakeDoneMsgMs = now; 
        }
        if (oven == OvenState::BAKE_DONE && now - bakeDoneMsgMs > Config::Hard::BAKE_DONE_MSG_MS) 
            oven = OvenState::PREHEAT;

        // [冷却管理] 誤判定防止のため、安定して低温であることを確認して終了
        static uint32_t coolStableStart = 0;
        bool cooledNow = (up.plateC < Config::Hard::COOL_COMPLETE_C && lo.plateC < Config::Hard::COOL_COMPLETE_C);
        if (!cooledNow) coolStableStart = 0;
        else if (coolStableStart == 0) coolStableStart = now;
        bool cooledConfirmed = (coolStableStart != 0 && now - coolStableStart > 2000UL);

        if (oven == OvenState::REST && (now - restStartMs > Config::Hard::REST_TIMEOUT_MS || cooledConfirmed)) {
            oven = OvenState::COOLING;
            bakeDoneMsgMs = now; // メッセージ表示タイマーとして再利用
        }
        else if (oven == OvenState::COOLING) {
            if (cooledConfirmed) {
                if (now - bakeDoneMsgMs > 3000UL) { 
                    oven = OvenState::SHUTDOWN; up.reset(); lo.reset(); coolStableStart = 0; dirtySave(true);
                }
            } else {
                bakeDoneMsgMs = now; // まだ熱い場合はタイマーをリセット（冷却完了から3秒後にOFFにするため）
            }
        }

        // [緊急停止] エラー発生時は全リセットし、安全リレーを遮断
        if (up.error || lo.error) {
            oven = OvenState::ERROR; up.reset(); lo.reset();
            targetUpPWM = 0; targetLoPWM = 0;
            digitalWrite(Config::Pins::SAFETY_RELAY, LOW);
            dirtySave(true);
            return;
        }

        // PWM値の再計算（1Hz）
        calculatePower(now);
    }
}

// シリアルプロッタ用テレメトリ出力
void debugTelemetry(uint32_t now) {
    static uint32_t lastLogMs = 0;
    if (now - lastLogMs >= 1000UL) {
        lastLogMs = now;
        bool isHeating = oven != OvenState::REST && oven != OvenState::COOLING &&
                         oven != OvenState::SHUTDOWN && oven != OvenState::ERROR;

        float upSet = (oven == OvenState::TUNING) ? Config::Hard::TUNE_TARGET_C : (isHeating ? currentRecipe.upC : 0);
        float loSet = (oven == OvenState::TUNING) ? Config::Hard::TUNE_TARGET_C : (isHeating ? currentRecipe.loC : 0);
        Serial.print(F("US:")); Serial.print(upSet);
        Serial.print(F(" LS:")); Serial.print(loSet);
        Serial.print(F(" UP:")); Serial.print(up.plateC);
        Serial.print(F(" LP:")); Serial.print(lo.plateC);
        Serial.print(F(" UH:")); Serial.print(up.heaterC);
        Serial.print(F(" LH:")); Serial.print(lo.heaterC);
        Serial.print(F(" UW:")); Serial.print(targetUpPWM);
        Serial.print(F(" LW:")); Serial.print(targetLoPWM);
        Serial.print(F(" SK:")); Serial.print(min(up.soak, lo.soak));
        Serial.print(F(" ST:")); Serial.print((int)oven);
        Serial.print(F(" LM:")); Config::Limit lim; memcpy_P(&lim, &Config::limits[settings.limitIdx], sizeof(lim)); Serial.println(lim.watts);
    }
}

void setup() {
    wdt_disable(); // 初期化中のリセットを防ぐ
    Serial.begin(115200);
    pinMode(Config::Pins::SAFETY_RELAY, OUTPUT); digitalWrite(Config::Pins::SAFETY_RELAY, LOW);
    pinMode(Config::Pins::ENC_CLK, INPUT_PULLUP); pinMode(Config::Pins::ENC_DT , INPUT_PULLUP); pinMode(Config::Pins::ENC_SW , INPUT_PULLUP);

    EEPROM.get(0, settings);
    if (settings.magic != Config::EEPROM_MAGIC) {
        // デフォルト値の設定（マジックナンバー排除）
        settings = {
            Config::EEPROM_MAGIC, 0, 0, 100.0f, 100.0f, 
            3.5f, 0.05f, 1.0f, // UP PID Default
            3.5f, 0.05f, 1.0f  // LO PID Default
        }; 
        EEPROM.put(0, settings);
    }
    lastSaveSettings = settings; // 初期状態を同期
    lastSavedUpHealth = settings.upHealth;
    lastSavedLoHealth = settings.loHealth;

    up.setTunings(settings.upKp, settings.upKi, settings.upKd);
    lo.setTunings(settings.loKp, settings.loKi, settings.loKd);
    memcpy_P(&currentRecipe, &Config::recipes[settings.recipeIdx], sizeof(currentRecipe));

    oled.begin(); // U8x8初期化
    
    // 隠し機能: 起動時にボタン長押しでチューニングモードへ
    if (digitalRead(Config::Pins::ENC_SW) == LOW) {
        askConfirmation = AskConfirmation::START_TUNE;
        confirmationYes = false;
        while(digitalRead(Config::Pins::ENC_SW) == LOW) { delay(10); } // ボタンが離されるまで待機（誤操作防止）
    }

    // ヒーター健康度（摩耗状態）を起動時に表示
    oled.clear();
    oled.setFont(u8x8_font_chroma48medium8_r);
    oled.setCursor(2, 0); oled.print(F("HEATER HEALTH"));
    oled.setCursor(0, 2); oled.print(F("UP : ")); oled.print(settings.upHealth, 1); oled.print(F("%"));
    oled.setCursor(0, 4); oled.print(F("LO : ")); oled.print(settings.loHealth, 1); oled.print(F("%"));
    // oled.display(); // U8x8は即時描画なので不要
    delay(2000);

    renderOLED(); 
    digitalWrite(Config::Pins::SAFETY_RELAY, HIGH); // 安全回路を通電
    lastActMs = millis(); 
    wdt_enable(WDTO_8S); // 8秒のウォッチドッグタイマーを設定
}

void loop() {
    wdt_reset(); 
    uint32_t now = millis();
    
    handleInput(now);
    runControlTick(now);

    if (oven == OvenState::ERROR) {
        up.drive(0);
        lo.drive(0);
    } else {
        if (oven == OvenState::TUNING && tuneStage == 1) targetLoPWM = 0;
        if (oven == OvenState::TUNING && tuneStage == 3) targetUpPWM = 0;
        up.drive(targetUpPWM); lo.drive(targetLoPWM);
    }

    updateDisplay(now);
    dirtySave(); // 必要に応じて保存実行
    debugTelemetry(now);
}