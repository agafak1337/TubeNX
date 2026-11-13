// md3/ui.cpp
#include "md3/ui.hpp"

#include "platform.hpp"

#include <cmath>

namespace tnx::ui {

uint64_t Ui::hash(const char* s, uint64_t seed) {
    uint64_t h = seed;
    while (*s) {
        h ^= (uint64_t)(unsigned char)*s++;
        h *= 1099511628211ull;
    }
    return h;
}

uint64_t Ui::id(const char* s, int extra) const {
    uint64_t h = hash(s);
    h ^= (uint64_t)extra + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h ? h : 1;
}

uint64_t Ui::idPtr(const void* p) const {
    uint64_t v = (uint64_t)p;
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdull;
    v ^= v >> 33;
    return v ? v : 1;
}

NodeState& Ui::node(uint64_t id) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        NodeState st;
        st.hover.setImmediate(0.f);
        st.press.setImmediate(0.f);
        st.focus.setImmediate(0.f);
        it = nodes_.emplace(id, st).first;
    }
    return it->second;
}

void Ui::beginFrame(Canvas& c, const md3::Theme& th, Fonts& f, const Input& in) {
    canvas_ = &c;
    theme_ = &th;
    fonts_ = &f;
    in_ = in;
    cands_.clear();
    hotId_ = 0;
    hoveredId_ = 0;
    activated_ = false;
    lastScrollConsume_ = 0.f;
    if (in_.released) dragScrollId_ = 0;
}

void Ui::focusable(uint64_t id, const Rect& r) {
    Cand cand;
    cand.id = id;
    cand.rect = r;
    cand.scrollId = scrollStack_.empty() ? 0 : scrollStack_.back();
    cands_.push_back(cand);
    // Запоминаем привязку к скролл-контейнеру в долгоживущем состоянии:
    // cands_ очищается в начале каждого кадра, а стик должен скроллить
    // контейнер с фокусом и на следующем кадре.
    node(id).scrollId = cand.scrollId;
}

bool Ui::interact(uint64_t id, const Rect& r) {
    NodeState& st = node(id);
    bool inside = in_.pointerVisible && r.contains(in_.pointer);
    bool focusedNow = (focusedId_ == id);

    float hoverTarget = inside ? 1.f : 0.f;
    st.hover.target = hoverTarget;
    st.hover.update(in_.dt);

    if (inside) {
        hoveredId_ = id;
        if (in_.pressed) hotId_ = id;
    }
    if (focusedNow && !in_.pointerVisible) {
        st.focus.target = 1.f;
    } else {
        st.focus.target = focusedNow ? 0.8f : 0.f;
    }
    st.focus.update(in_.dt);

    bool act = false;

    // Свайп по карточке не должен превращаться в тап: как только палец
    // реально пошёл, снимаем «активную» кнопку — на отпускании активации
    // уже не будет, зато контент прокрутится.
    if (in_.dragging && activeId_ == id) activeId_ = 0;

    if (in_.pressed && inside) {
        activeId_ = id;
        st.rippleStart = in_.time;
        st.rippleOrigin = in_.pointer;
    }
    if (in_.released) {
        if (activeId_ == id && inside) act = true;
        if (activeId_ == id) activeId_ = 0;
    }
    if (focusedNow && in_.accept) {
        act = true;
        st.rippleStart = in_.time;
        st.rippleOrigin = {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    }
    if (focusedNow && in_.accept) {
        // визуальный отклик на A
        st.press.target = 1.f;
    }
    st.press.target = (activeId_ == id) ? 1.f : 0.f;
    st.press.update(in_.dt);

    focusable(id, r);

    // Автопрокрутка к элементу, получившему фокус с геймпада.
    if (act && focusedNow) {
        for (auto& c : cands_) {
            if (c.id == id && c.scrollId) {
                auto sit = scrolls_.find(c.scrollId);
                if (sit != scrolls_.end()) {
                    auto& s = sit->second;
                    float off = s.offset.target;
                    if (c.rect.y - off < s.viewport.y + 8.f) s.offset.target = c.rect.y - s.viewport.y - 8.f;
                    if (c.rect.b() - off > s.viewport.b() - 8.f)
                        s.offset.target = c.rect.b() - s.viewport.b() + 8.f;
                    s.offset.target = std::clamp(s.offset.target, 0.f, std::max(0.f, s.maxOffset));
                }
            }
        }
    }

    if (act) {
        activated_ = true;
        platform::bootLog("ui: activate id=%016llx", (unsigned long long)id);
    }
    return act;
}

bool Ui::hovered(uint64_t id) { return hoveredId_ == id; }
float Ui::hoverAmount(uint64_t id) { return node(id).hover.value; }
float Ui::pressAmount(uint64_t id) { return node(id).press.value; }
float Ui::focusAmount(uint64_t id) { return node(id).focus.value; }

void Ui::ripple(uint64_t id, const Rect& shape, const CornerRadii& cr, const Color& c) {
    // Во время свайпа волна не нужна (и она дорогая — SDF по всей площади).
    if (in_.dragging || in_.dragged) return;
    NodeState& st = node(id);
    float t = (in_.time - st.rippleStart) / 0.45f;
    if (t < 0.f || t > 1.f) return;
    float maxR = std::sqrt(shape.w * shape.w + shape.h * shape.h);
    float radius = maxR * (0.35f + 0.65f * (1.f - (1.f - t) * (1.f - t)));
    float alpha = (1.f - t) * (1.f - t);
    canvas_->pushClip(shape);
    canvas_->ripple(shape, cr, st.rippleOrigin, radius, c.withAlpha(alpha));
    canvas_->popClip();
}

void Ui::stateLayer(uint64_t id, const Rect& shape, const CornerRadii& cr, const Color& tint,
                    float baseOpacity) {
    NodeState& st = node(id);
    float op = baseOpacity + st.hover.value * 0.08f + st.press.value * 0.10f + st.focus.value * 0.06f;
    op = std::min(op, 0.24f);
    if (op > 0.002f) canvas_->fillRoundRect(shape, cr, tint.withAlpha(op));
}

float Ui::scrollArea(uint64_t id, const Rect& viewport, float contentHeight, bool* nearEnd) {
    ScrollState& s = scrolls_[id];
    s.viewport = viewport;
    s.maxOffset = std::max(0.f, contentHeight - viewport.h);
    if (!s.inited) {
        s.inited = true;
        s.offset.setImmediate(0.f);
    }
    bool inside = in_.pointerVisible && viewport.contains(in_.pointer);
    // cands_ очищается в beginFrame, поэтому привязку фокуса к контейнеру
    // берём из nodes_ — иначе стик не мог прокрутить список без мыши.
    bool focusedInside = false;
    auto fn = nodes_.find(focusedId_);
    if (fn != nodes_.end() && fn->second.scrollId == id) focusedInside = true;
    if ((inside || focusedInside) && std::fabs(in_.scrollY) > 0.01f) {
        s.offset.target += in_.scrollY;
        lastScrollConsume_ = in_.scrollY;
    }

    // Тач-свайп: палец «держит» контент, поэтому двигаем и target, и value
    // (иначе пружина догоняла бы палец с задержкой и всё равно лагало бы).
    if (in_.pressed && inside) dragScrollId_ = id;
    if (in_.dragging && dragScrollId_ == id) {
        s.offset.target -= in_.dragY;
        s.offset.value -= in_.dragY;
        s.offset.velocity = 0.f;
        lastScrollConsume_ = -in_.dragY;
    }

    s.offset.target = std::clamp(s.offset.target, 0.f, s.maxOffset);
    s.offset.update(in_.dt);
    s.offset.value = std::clamp(s.offset.value, 0.f, s.maxOffset);
    if (nearEnd) *nearEnd = (s.maxOffset > 0.f) && (s.offset.value > s.maxOffset - 240.f);
    return s.offset.value;
}

void Ui::scrollTo(uint64_t id, float offset) {
    ScrollState& s = scrolls_[id];
    s.offset.target = std::clamp(offset, 0.f, s.maxOffset);
    if (!s.inited) s.offset.setImmediate(s.offset.target);
}

bool Ui::scrolling(uint64_t id) const {
    auto it = scrolls_.find(id);
    if (it == scrolls_.end()) return false;
    return std::fabs(it->second.offset.velocity) > 8.f;
}

void Ui::moveFocus(int dx, int dy) {
    if (cands_.empty()) return;
    Rect cur{};
    bool haveCur = false;
    for (auto& c : cands_) {
        if (c.id == focusedId_) { cur = c.rect; haveCur = true; break; }
    }
    Vec2 cc = haveCur ? cur.center() : Vec2(0, 0);
    uint64_t best = 0;
    float bestScore = 1e18f;
    for (auto& c : cands_) {
        if (c.id == focusedId_) continue;
        Vec2 p = c.rect.center();
        float vx = p.x - cc.x, vy = p.y - cc.y;
        float along = vx * dx + vy * dy;
        if (along <= 4.f) continue;
        float perp = std::fabs(vx * dy - vy * dx);
        float score = along + perp * 2.2f;
        if (score < bestScore) { bestScore = score; best = c.id; }
    }
    if (!best) {
        // Фолбэк: ближайший по расстоянию.
        float bd = 1e18f;
        for (auto& c : cands_) {
            if (c.id == focusedId_) continue;
            Vec2 p = c.rect.center();
            float d = (p.x - cc.x) * (p.x - cc.x) + (p.y - cc.y) * (p.y - cc.y);
            if (d < bd) { bd = d; best = c.id; }
        }
    }
    if (!best) return;
    focusedId_ = best;
    // Прокрутить контейнер к новому фокусу.
    for (auto& c : cands_) {
        if (c.id != best || !c.scrollId) continue;
        auto it = scrolls_.find(c.scrollId);
        if (it == scrolls_.end()) break;
        auto& s = it->second;
        float top = c.rect.y - viewportTopOf(c.scrollId);
        float off = s.offset.target;
        if (c.rect.y - off < s.viewport.y + 10.f) s.offset.target = c.rect.y - s.viewport.y - 10.f;
        if (c.rect.b() - off > s.viewport.b() - 10.f) s.offset.target = c.rect.b() - s.viewport.b() + 10.f;
        s.offset.target = std::clamp(s.offset.target, 0.f, std::max(0.f, s.maxOffset));
        (void)top;
        break;
    }
}

float Ui::viewportTopOf(uint64_t scrollId) const {
    auto it = scrolls_.find(scrollId);
    return it == scrolls_.end() ? 0.f : it->second.viewport.y;
}

void Ui::endFrame() {
    if (in_.left) moveFocus(-1, 0);
    else if (in_.right) moveFocus(1, 0);
    else if (in_.up) moveFocus(0, -1);
    else if (in_.downEdge) moveFocus(0, 1);

    // Стабильный первый фокус, чтобы геймпад работал сразу.
    if (!focusedId_ && !cands_.empty() && in_.pointerVisible == false) {
        focusedId_ = cands_.front().id;
    }
    // Отсеиваем пропавшие элементы.
    if (focusedId_) {
        bool found = false;
        for (auto& c : cands_)
            if (c.id == focusedId_) { found = true; break; }
        if (!found) focusedId_ = 0;
    }
    // Выкидываем давно неиспользуемые состояния.
    if (nodes_.size() > 4000) nodes_.clear();
}

} // namespace tnx::ui