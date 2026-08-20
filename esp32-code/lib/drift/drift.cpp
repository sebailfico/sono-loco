#include "drift.h"

void DriftController::begin(const Config &cfg, uint32_t nowMs) {
    cfg_ = cfg;
    inserted = 0;
    dropped  = 0;
    reset(nowMs);
}

void DriftController::reset(uint32_t nowMs) {
    ema_    = 0.0f;
    rate_   = 0.0f;
    credit_ = 0.0f;
    lastMs_ = nowMs;
    seeded_ = false;
}

DriftController::Correction DriftController::update(uint32_t nowMs, int fillBytes) {
    // Unsigned subtraction, so this is correct across the millis() wrap at
    // 49 days. A node is expected to play for longer than that.
    uint32_t dt = nowMs - lastMs_;
    lastMs_ = nowMs;

    if (!seeded_) {
        // Seed the filter with the first reading instead of ramping up from
        // zero, which would otherwise look like a buffer 4000 bytes too empty
        // and provoke a burst of corrections at every restart.
        ema_    = (float)fillBytes;
        seeded_ = true;
        return NONE;
    }

    if (dt > MAX_DT_MS) dt = MAX_DT_MS;
    const float dtSec = (float)dt / 1000.0f;

    // Single-pole low-pass, in the dt form so the time constant is honest even
    // though the caller's batch interval is not exactly regular.
    const float alpha = (float)dt / (cfg_.emaTauMs + (float)dt);
    ema_ += alpha * ((float)fillBytes - ema_);

    const float error = ema_ - (float)cfg_.targetBytes;
    const float dead  = (float)cfg_.deadbandBytes;

    float excess = 0.0f;
    if (error > dead)       excess = error - dead;
    else if (error < -dead) excess = error + dead;

    rate_ = cfg_.kp * excess;
    if (rate_ >  cfg_.maxRatePerSec) rate_ =  cfg_.maxRatePerSec;
    if (rate_ < -cfg_.maxRatePerSec) rate_ = -cfg_.maxRatePerSec;

    credit_ += rate_ * dtSec;

    // Credit is not spent here -- confirm() does that. Reporting the intent
    // repeatedly until it is applied is deliberate: a correction the caller
    // could not make is still owed.
    if (credit_ >=  1.0f) return DROP;
    if (credit_ <= -1.0f) return INSERT;
    return NONE;
}

void DriftController::confirm(Correction applied) {
    if (applied == DROP) {
        credit_ -= 1.0f;
        dropped++;
    } else if (applied == INSERT) {
        credit_ += 1.0f;
        inserted++;
    }
}
