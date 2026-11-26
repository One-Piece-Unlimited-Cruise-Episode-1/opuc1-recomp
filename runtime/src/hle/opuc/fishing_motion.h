// Wii Remote motion for PC fishing, ported from the Play tab (programs/apps/shared/web/input/wiimotion.js and the
// Simple-input constants of programs/apps/play/web/play/fishing.js). Not in the game: the game reads the remote's
// accelerometer through WiiMotion (CharaFishing +0x130, vault entities/pad-input "Remote motion"); these timelines
// are what a keyboard / mouse player sends instead of a real swing.
//
// Frame: KPAD acc in g. A remote held level, pointing at the screen, reads REST = (0, -1, 0); raising the tip moves
// toward z -1 (the fishing pitch); rolling moves x (the lean). About 3.3 samples per 60 Hz frame (KPAD at 200 Hz).
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace OpucFishing {

struct V3 { float x = 0, y = 0, z = 0; };
inline constexpr V3 kRest{0.f, -1.f, 0.f};
inline constexpr double kSamplesPerFrame = 200.0 / 60.0;
inline float Len(const V3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

struct Step { double ms; V3 v; };
using Timeline = std::vector<Step>;

// A rod throw: the tip back over the shoulder (a light pull, pitch up), then the forward flick with |acc| ~ g for
// `ms`, then level. The cast needs ema * decay * 0.25 >= 12 with A released within 12 samples of the flick's end.
inline Timeline RodThrow(float g = 4.f, double ms = 160, double back = 300) {
    return {{back, {0.f, -0.2f, -1.2f}}, {ms, {0.f, -0.6f, -g}}, {40, {0.f, -1.4f, 0.6f}}};
}
// The hook: a short sharp pull of the tip (WiiMotion detector 0: |acc| > 2 with y > 1.7). Keep it short: a gesture
// above 2.0 g for more than ~8 samples is published through the 28-sample path and overwritten (Dolphin: 40 ms
// jerks hooked 7 / 7, 100 ms ones 1 / 4).
inline Timeline RodJerk(float g = 2.6f, double ms = 40) { return {{ms, {0.f, g, -1.0f}}}; }
// The resting remote at `pitch` (0 level ... 1 pointing up) rolled by `lean` (-1 ... 1; |x| 0.3 g is a full lean:
// lean = sign(-10x) * clamp(|10x| - 2, 0, 1)).
inline V3 TiltAccel(float pitch, float lean) {
    const float a = std::clamp(pitch, 0.f, 1.f) * 3.14159265f / 2.f;
    const float x = lean != 0.f ? -(lean > 0 ? 1.f : -1.f) * (0.2f + 0.1f * std::min(1.f, std::fabs(lean))) : 0.f;
    const float c = std::sqrt(std::max(0.f, 1.f - x * x));
    return {x, -std::cos(a) * c, -std::sin(a) * c};
}

// Where the samples come from: a timeline, a held vector (tilt) or the mouse (Remote input: the tip follows the
// mouse, vertical = pitch, horizontal = roll; a point at arm's length L gives tangential L*alpha and centripetal
// L*omega^2).
class MotionSource {
public:
    void Set(const V3& v) { held_ = v; timeline_.clear(); }
    void Play(const Timeline& steps, const V3& end = kRest) {
        timeline_.clear();
        double at = 0;
        for (const Step& s : steps) { timeline_.push_back({at, s.v}); at += s.ms; }
        timeline_.push_back({at, end});
        ms_ = 0; end_ = at; held_ = end;
    }
    bool Playing() const { return !timeline_.empty(); }
    bool MouseOn() const { return mouseOn_; }
    // Mouse movement of this frame in radians (Remote input). Idle for half a second, the tilt takes over again.
    void MouseMove(double dx, double dy) {
        if (dx == 0 && dy == 0) { if (mouseOn_ && ++idle_ > 30) mouseOn_ = false; return; }
        idle_ = 0;
        const double k = 1.6, p0 = pitch_;
        pitch_ = std::clamp(pitch_ - dy * k, -0.6, 1.6);
        w_ = (pitch_ - p0) * 60;
        roll_ = std::clamp(roll_ + dx * k, -0.7, 0.7);
        mouseOn_ = true;
    }
    void ResetMouse() { pitch_ = roll_ = w_ = wPrev_ = 0; idle_ = 0; mouseOn_ = false; }
    // The n samples of one frame (1/60 s), oldest first.
    void Frame(int n, std::vector<V3>& out) {
        out.clear();
        if (!timeline_.empty()) {
            for (int i = 0; i < n; ++i) {
                const double ms = ms_ + (i + 1) * (1000.0 / 60.0) / n;
                V3 v = timeline_.back().v;
                for (size_t k = timeline_.size(); k-- > 0;) if (ms >= timeline_[k].at) { v = timeline_[k].v; break; }
                out.push_back(v);
            }
            ms_ += 1000.0 / 60.0;
            if (ms_ >= end_) timeline_.clear();
            return;
        }
        if (mouseOn_) {
            const double L = 0.6 / 9.8, a = pitch_, alpha = (w_ - wPrev_) * 60;
            const V3 v{static_cast<float>(-std::sin(roll_)),
                       static_cast<float>(-std::cos(a) * std::cos(roll_) + L * w_ * w_),
                       static_cast<float>(-std::sin(a) * std::cos(roll_) - L * alpha * 0.25)};
            for (int i = 0; i < n; ++i) out.push_back(v);
            wPrev_ = w_; w_ *= 0.5; roll_ *= 0.97;
            return;
        }
        for (int i = 0; i < n; ++i) out.push_back(held_);
    }

private:
    struct Key { double at; V3 v; };
    V3 held_ = kRest;
    std::vector<Key> timeline_;
    double ms_ = 0, end_ = 0;
    double pitch_ = 0, roll_ = 0, w_ = 0, wPrev_ = 0;
    int idle_ = 0;
    bool mouseOn_ = false;
};

// The part of the game's WiiMotion that the cast reads: the 60-sample |acc| ring and AnalyzeSwing 80025494.
class SwingRing {
public:
    void Sample(const V3& v) { ring_[idx_] = Len(v); idx_ = (idx_ + 1) % 60; }
    // power = ema * decay; the cast uses power * 0.25 when quiet < 12, else 0.
    double CastPower() const {
        double ema = 0, decay = 1;
        int count = 0, quiet = 0;
        for (int k = 0; k < 60; ++k) {
            const float s = ring_[(idx_ - 1 - k + 120) % 60];
            if (s <= 1.5f) { if (count) break; decay *= 0.9; quiet++; }
            else { count++; ema = 0.98 * ema + s; }
        }
        return quiet < 12 ? ema * decay * 0.25 : 0.0;
    }

private:
    float ring_[60] = {};
    int idx_ = 0;
};

// Simple input (Play tab fishing.js): holding the cast key fills a power bar that swings 0 -> 1 -> 0 over
// kChargeFrames * 2; releasing plays a throw with A held through it and let go in the follow-through, so the game's
// AnalyzeSwing power and cast speed formula decide the cast. The throw by effort e (bar * the rod's top effort): up
// to e = 1 a 3.4 g flick lasting 100 + 200 * e ms, past 1 the 300 ms flick at 3.4 * e g. ^[viewer input mapping]
inline constexpr int kMinCharge = 8, kChargeFrames = 60;
inline constexpr double kSimpleG = 3.4, kSimpleBack = 150, kSimpleMsMin = 100, kSimpleMsSpan = 200;
inline constexpr double kSimpleOver = 1.15, kEffortMin = 0.3, kEffortMax = 12;
struct Throw { double g, back, ms; };
inline Throw SimpleThrow(double e) {
    return {kSimpleG * std::max(1.0, e), kSimpleBack, std::round(kSimpleMsMin + kSimpleMsSpan * std::min(1.0, e))};
}
inline int ThrowFrames(const Throw& t) { return static_cast<int>(std::lround((t.back + t.ms + 10) * 60 / 1000)); }
inline Timeline ThrowTimeline(const Throw& t) { return RodThrow(static_cast<float>(t.g), t.ms, t.back); }
inline double PingPong(double t) { const double x = std::fmod(t, 2.0); return x < 1 ? x : 2 - x; }
// The Remote-input throw (the Shake key with A held in the wind-up): input/wiimotion.js rodThrow defaults.
inline constexpr Throw kRemoteThrow{4.0, 300, 160};

// The game's cast power for a Simple throw of effort e: the timeline at 200 Hz through the ring, A let go after
// ThrowFrames (the same samples the runtime sends).
inline double SimpleCastPower(double e) {
    const Throw t = SimpleThrow(e);
    MotionSource m;
    m.Play(ThrowTimeline(t));
    SwingRing ring;
    std::vector<V3> s;
    double acc = 0;
    for (int f = 0; f < ThrowFrames(t); ++f) {
        acc += kSamplesPerFrame;
        const int n = static_cast<int>(acc);
        acc -= n;
        m.Frame(n, s);
        for (const V3& v : s) ring.Sample(v);
    }
    return ring.CastPower();
}
// CharaFishing::Update state 0: speed = cp * ((0.5 + 1.4286 * max(0, 50 - cp) / 100) * (power - 12) / 15) + 15
// (cp = rod_prm +4), only for power >= 12.
inline double CastSpeed(double power, double cp) {
    if (power < 12) return 0;
    return cp * ((0.5 + 1.4286 * std::max(0.0, 50 - cp) / 100) * (power - 12) / 15) + 15;
}
// The lure flight fitted to Dolphin (Play tab fishing.js FLIGHT): horizontal speed x 0.99 a frame, gravity 48,
// launch 0.95 * speed at 45 deg from 3.0 above the feet and 0.3 ahead, until it falls to dy (water below the feet).
// ^[inferred: the game's line simulation 80032c64 / 80032b08 is not traced]
inline double FlightDistance(double speed, double dy) {
    const double v = 0.95 * speed * std::sqrt(0.5);
    double x = 0.3, y = 3.0, vx = v, vy = v;
    for (int i = 0; i < 600; ++i) {
        vx *= 0.99; vy = vy - 48.0 / 60; x += vx / 60; y += vy / 60;
        if (vy < 0 && y <= dy) break;
    }
    return std::hypot(x, y);
}

}  // namespace OpucFishing
