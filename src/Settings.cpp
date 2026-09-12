#include "Settings.h"

#include "AppPaths.h"

#include <algorithm>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "misc/cpp/imgui_stdlib.h"

namespace Settings {
namespace {

// Enough for the fields plus the buttons; the user can resize, and the window
// manager gets the last word as it does for the pet.
constexpr int  kWidth       = 520;
constexpr int  kHeight      = 420;
constexpr float kFontSize   = 15.0f;
constexpr float kLabelWidth = 140.0f;
constexpr float kButtonW    = 82.0f;

// One spelling, because the banner is measured and drawn from it and a height
// computed from different text than it renders is a clipped button.
const char* const kBannerText =
    "Size / position was adjusted while hidden. Window will stay hidden "
    "after changes are saved or cancelled.";

struct State {
    SDL_Window*   window   = nullptr;
    SDL_Renderer* renderer = nullptr;
    ImGuiContext* ctx      = nullptr;

    Spec               spec;
    std::vector<Field> fields;     // what the widgets are editing
    std::vector<Field> baseline;   // last applied, or as opened: what Apply is
                                   // enabled against and what dirty() compares

    // What on_change last saw, per field, so an edit is reported once rather
    // than every frame it stays different.
    std::vector<std::string> reported;

    std::vector<std::string> tabs;      // in order of first appearance
    std::string              error;     // returned by on_apply; shown, not swallowed
    bool                     banner = false;
    bool                     closing = false;

    std::function<void()> on_preview_show;
};

State  g;
bool   g_open = false;

// The one spelling of a field's value, used for the change callback, for the
// apply payload and for the dirty comparison -- so those three can never
// disagree about whether something changed.
std::string valueOf(const Field& f)
{
    switch (f.kind) {
    case Field::Kind::Int:
    case Field::Kind::Range:  return std::to_string(f.int_value);
    case Field::Kind::Bool:   return f.bool_value ? "true" : "false";
    case Field::Kind::Choice:
        if (f.choice_index >= 0 && f.choice_index < (int)f.choices.size())
            return f.choices[(size_t)f.choice_index];
        return std::string();
    case Field::Kind::Text:
    default:                  return f.text_value;
    }
}

// Which window an event belongs to, or 0 for one that belongs to none.
//
// SDL3 puts the window id in a different member of the union per event type,
// and they are not interchangeable. Getting this wrong would hand the pet a
// click that landed on a dialog, or the other way round.
SDL_WindowID eventWindow(const SDL_Event& e)
{
    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:        return e.motion.windowID;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:     return e.button.windowID;
    case SDL_EVENT_MOUSE_WHEEL:         return e.wheel.windowID;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:              return e.key.windowID;
    case SDL_EVENT_TEXT_INPUT:          return e.text.windowID;
    case SDL_EVENT_TEXT_EDITING:        return e.edit.windowID;
    default:
        if (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST)
            return e.window.windowID;
        return 0;
    }
}

void collectTabs()
{
    g.tabs.clear();
    for (const Field& f : g.fields) {
        const std::string name = f.tab.empty() ? "General" : f.tab;
        if (std::find(g.tabs.begin(), g.tabs.end(), name) == g.tabs.end())
            g.tabs.push_back(name);
    }
}

void teardown()
{
    if (g.ctx) {
        ImGui::SetCurrentContext(g.ctx);
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(g.ctx);
        g.ctx = nullptr;
    }
    if (g.renderer) { SDL_DestroyRenderer(g.renderer); g.renderer = nullptr; }
    if (g.window)   { SDL_DestroyWindow(g.window);     g.window   = nullptr; }

    g.fields.clear();
    g.baseline.clear();
    g.reported.clear();
    g.tabs.clear();
    g.error.clear();
    g.banner  = false;
    g.closing = false;
    g_open    = false;
}

void tooltip(const Field& f)
{
    if (f.help.empty()) return;
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted(f.help.c_str());
        ImGui::EndTooltip();
    }
}

// Width of the number box beside a range slider. Five digits plus the frame:
// every bound this dialog deals in is a screen coordinate.
constexpr float kRangeBoxW = 66.0f;

void clampToBounds(Field& f)
{
    if (f.int_min != f.int_max)
        f.int_value = std::max(f.int_min, std::min(f.int_max, f.int_value));
}

// The mouse wheel, over a range that is hovered or being typed in.
//
// SetItemKeyOwner is what stops the wheel doing two things at once: without it
// the field list under the cursor scrolls at the same time, and the value the
// user was aiming at slides out from under the pointer. Claiming the wheel for
// the hovered item is ImGui's own mechanism for exactly this.
//
// One unit per notch, ten with Shift. Deliberately not scaled to the range: a
// slider that spans a whole screen is the coarse control, and the reason to
// reach for the wheel is that you want the pixel you want.
bool wheelAdjust(Field& f, bool over)
{
    if (!over) return false;
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);

    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel == 0.0f) return false;

    const long long step = ImGui::GetIO().KeyShift ? 10 : 1;
    f.int_value += (long long)(wheel > 0.0f ? step : -step);
    clampToBounds(f);
    return true;
}

// A slider and a number box that edit the same value.
//
// Returns true while the user is still inside the box: a half-typed number is
// not an edit yet, and reporting it would send the window to 1, then 16, then
// 161 on the way to 1612. The slider does not get that treatment -- dragging it
// is meant to be watched, so every position it passes through is reported.
bool drawRange(Field& f)
{
    const float box = kRangeBoxW + ImGui::GetStyle().ItemSpacing.x;

    ImGui::SetNextItemWidth(-box);
    // An empty format leaves the number to the box beside it rather than
    // printing it twice.
    ImGui::SliderScalar("##slider", ImGuiDataType_S64, &f.int_value,
                        &f.int_min, &f.int_max, "");
    const bool over_slider = ImGui::IsItemHovered();
    const bool dragging    = ImGui::IsItemActive();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(kRangeBoxW);
    if (ImGui::InputScalar("##box", ImGuiDataType_S64, &f.int_value,
                           nullptr, nullptr, "%lld"))
        clampToBounds(f);
    const bool typing  = ImGui::IsItemActive();
    const bool over_box = ImGui::IsItemHovered();

    // The wheel works over either half, and while the box has the caret, so
    // "click in the box then scroll" does what it looks like it should.
    wheelAdjust(f, over_slider || over_box || typing);

    (void)dragging;
    return typing;
}

// Draw one field and say whether the user is still inside it. A value being
// typed is not an edit yet -- see reportChanges().
bool drawField(Field& f)
{
    ImGui::PushID(f.key.c_str());

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(f.label.c_str());
    tooltip(f);
    ImGui::SameLine(kLabelWidth);
    ImGui::SetNextItemWidth(-FLT_MIN);

    switch (f.kind) {
    case Field::Kind::Range: {
        const bool typing = drawRange(f);
        ImGui::PopID();
        return typing;
    }
    case Field::Kind::Int: {
        const long long step = 1, step_fast = 10;
        if (ImGui::InputScalar("", ImGuiDataType_S64, &f.int_value,
                               &step, &step_fast, "%lld"))
            clampToBounds(f);
        break;
    }
    case Field::Kind::Bool:
        ImGui::Checkbox("", &f.bool_value);
        break;
    case Field::Kind::Choice: {
        const char* preview = (f.choice_index >= 0 &&
                               f.choice_index < (int)f.choices.size())
                            ? f.choices[(size_t)f.choice_index].c_str() : "";
        if (ImGui::BeginCombo("", preview)) {
            for (int i = 0; i < (int)f.choices.size(); ++i) {
                const bool selected = (i == f.choice_index);
                if (ImGui::Selectable(f.choices[(size_t)i].c_str(), selected))
                    f.choice_index = i;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        break;
    }
    case Field::Kind::Text:
    default:
        ImGui::InputText("", &f.text_value);
        break;
    }

    const bool active = ImGui::IsItemActive();
    ImGui::PopID();
    return active;
}

// Fire on_change for every live field whose value has settled on something new.
//
// "Settled" is what the active flag is for. InputScalar reports an edit on each
// keystroke, so reporting immediately would send the pet to x=1, then 16, then
// 161 on the way to typing 1612. Waiting until the field is no longer being
// typed in means Enter, Tab or a click elsewhere commits it -- while the +/-
// step buttons, which are never active for longer than a frame, still move the
// window the moment they are pressed.
void reportChanges(const std::vector<bool>& active)
{
    if (!g.spec.on_change) return;

    for (size_t i = 0; i < g.fields.size(); ++i) {
        if (!g.fields[i].live || active[i]) continue;

        const std::string now = valueOf(g.fields[i]);
        if (now == g.reported[i]) continue;

        g.reported[i] = now;
        g.spec.on_change(g.fields[i]);
    }
}

// OK and Apply share this. Returns true if the config was written -- or if
// there was nothing to write, which is the same answer to "may this close".
bool applyNow()
{
    if (!dirty()) return true;

    g.error.clear();
    if (g.spec.on_apply) {
        g.error = g.spec.on_apply(g.fields);
        if (!g.error.empty()) return false;
    }

    // The applied values become what Cancel would return to and what Apply is
    // measured against, which is the ordinary OK/Cancel/Apply contract: Apply
    // commits, and a later Cancel closes rather than undoing it.
    g.baseline = g.fields;
    return true;
}

// How tall the banner has to be for the text it is about to wrap, plus the
// button under it. Measured rather than guessed: the text wraps to however many
// lines the window's width gives it, and a fixed height is a Show button cut in
// half the moment somebody narrows the dialog.
float bannerHeight(float outer_width)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    const float inner = outer_width - st.WindowPadding.x * 2.0f;
    const ImVec2 text = ImGui::CalcTextSize(kBannerText, nullptr, false, inner);
    return text.y + st.ItemSpacing.y + ImGui::GetFrameHeight()
                  + st.WindowPadding.y * 2.0f;
}

void drawBanner(float height)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.24f, 0.20f, 0.10f, 1.0f));
    ImGui::BeginChild("##banner", ImVec2(0, height),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);

    ImGui::TextWrapped("%s", kBannerText);

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         ImGui::GetContentRegionAvail().x - kButtonW);
    if (ImGui::Button("Show", ImVec2(kButtonW, 0)) && g.on_preview_show)
        g.on_preview_show();

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void drawWindow()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    ImGui::Begin("##settings", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Everything below the field area, measured rather than guessed, so the
    // banner appearing shrinks the fields instead of pushing the buttons off
    // the bottom of the window.
    const float banner_h = g.banner
        ? bannerHeight(ImGui::GetContentRegionAvail().x) : 0.0f;

    float reserved = ImGui::GetFrameHeightWithSpacing()           // buttons
                   + ImGui::GetStyle().ItemSpacing.y * 2.0f;
    if (g.banner)          reserved += banner_h + ImGui::GetStyle().ItemSpacing.y;
    if (!g.error.empty())  reserved += ImGui::GetTextLineHeightWithSpacing();

    std::vector<bool> active(g.fields.size(), false);

    ImGui::BeginChild("##fields", ImVec2(0, -reserved));
    if (ImGui::BeginTabBar("##tabs")) {
        for (const std::string& tab : g.tabs) {
            if (!ImGui::BeginTabItem(tab.c_str())) continue;
            ImGui::Spacing();
            for (size_t i = 0; i < g.fields.size(); ++i) {
                const std::string name = g.fields[i].tab.empty() ? "General"
                                                                 : g.fields[i].tab;
                if (name != tab) continue;
                active[i] = drawField(g.fields[i]);
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    if (g.banner) drawBanner(banner_h);

    if (!g.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.40f, 1.0f));
        ImGui::TextWrapped("%s", g.error.c_str());
        ImGui::PopStyleColor();
    }

    // --- OK / Cancel / Apply, right-aligned ---------------------------------
    const float bw = kButtonW;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - (bw * 3 + gap * 2)
                         - ImGui::GetStyle().WindowPadding.x);

    if (ImGui::Button("OK", ImVec2(bw, 0))) {
        if (applyNow()) g.closing = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(bw, 0))) {
        if (g.spec.on_cancel) g.spec.on_cancel();
        g.closing = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!dirty());
    if (ImGui::Button("Apply", ImVec2(bw, 0))) applyNow();
    ImGui::EndDisabled();

    ImGui::End();

    reportChanges(active);
}

} // namespace

bool open() { return g_open; }

namespace {

// The field for a key, and its index, so a setter can suppress the echo by
// updating what on_change last saw alongside the value itself.
Field* findMutable(const std::string& key, size_t* index)
{
    for (size_t i = 0; i < g.fields.size(); ++i) {
        if (g.fields[i].key != key) continue;
        if (index) *index = i;
        return &g.fields[i];
    }
    return nullptr;
}

// Shared tail of every setter: remember the new value as already-reported, so
// the change the host was told about does not come back to Python as a change
// the user made.
void accept(size_t index)
{
    if (index < g.reported.size()) g.reported[index] = valueOf(g.fields[index]);
}

} // namespace

const Field* find(const std::string& key)
{
    return g_open ? findMutable(key, nullptr) : nullptr;
}

bool setInt(const std::string& key, long long value)
{
    size_t i = 0;
    Field* f = g_open ? findMutable(key, &i) : nullptr;
    if (!f || (f->kind != Field::Kind::Int && f->kind != Field::Kind::Range))
        return false;
    f->int_value = value;
    clampToBounds(*f);
    accept(i);
    return true;
}

bool setText(const std::string& key, const std::string& value)
{
    size_t i = 0;
    Field* f = g_open ? findMutable(key, &i) : nullptr;
    if (!f || f->kind != Field::Kind::Text) return false;
    f->text_value = value;
    accept(i);
    return true;
}

bool setBool(const std::string& key, bool value)
{
    size_t i = 0;
    Field* f = g_open ? findMutable(key, &i) : nullptr;
    if (!f || f->kind != Field::Kind::Bool) return false;
    f->bool_value = value;
    accept(i);
    return true;
}

bool setChoice(const std::string& key, const std::string& value)
{
    size_t i = 0;
    Field* f = g_open ? findMutable(key, &i) : nullptr;
    if (!f || f->kind != Field::Kind::Choice) return false;
    for (size_t c = 0; c < f->choices.size(); ++c) {
        if (f->choices[c] != value) continue;
        f->choice_index = (int)c;
        accept(i);
        return true;
    }
    return false;   // not one of the offered choices
}

bool dirty()
{
    if (g.fields.size() != g.baseline.size()) return true;
    for (size_t i = 0; i < g.fields.size(); ++i) {
        if (valueOf(g.fields[i]) != valueOf(g.baseline[i])) return true;
    }
    return false;
}

void setPreviewBanner(bool shown)        { g.banner = shown; }
void setOnPreviewShow(std::function<void()> fn) { g.on_preview_show = std::move(fn); }

bool show(const Spec& spec, std::string& error_out)
{
    if (g_open) {
        error_out = "the settings window is already open";
        return false;
    }

    // Always on top, because the pet is: a settings window the pet sits in
    // front of would be a dialog you have to move the thing you are
    // configuring to read.
    g.window = SDL_CreateWindow(spec.title.c_str(), kWidth, kHeight,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALWAYS_ON_TOP);
    if (!g.window) {
        error_out = std::string("SDL_CreateWindow (settings): ") + SDL_GetError();
        return false;
    }

    g.renderer = SDL_CreateRenderer(g.window, nullptr);
    if (!g.renderer) {
        error_out = std::string("SDL_CreateRenderer (settings): ") + SDL_GetError();
        SDL_DestroyWindow(g.window);
        g.window = nullptr;
        return false;
    }
    SDL_SetRenderVSync(g.renderer, 1);

    g.ctx = ImGui::CreateContext();
    if (!g.ctx) {
        error_out = "ImGui::CreateContext failed";
        SDL_DestroyRenderer(g.renderer); g.renderer = nullptr;
        SDL_DestroyWindow(g.window);     g.window   = nullptr;
        return false;
    }
    ImGui::SetCurrentContext(g.ctx);

    ImGuiIO& io = ImGui::GetIO();
    // No imgui.ini: the window is opened from a tray menu and closed again,
    // and a dotfile remembering which tab was last open is not worth writing
    // into the user's working directory.
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    // The face the pet already draws with, rather than ImGui's built-in
    // bitmap font. A missing one is not fatal -- ImGui falls back to its own --
    // but it is said out loud, because a shipped asset that is not there means
    // something is wrong with the package.
    const std::string font = AppPaths::asset("JetBrainsMono.ttf");
    if (!io.Fonts->AddFontFromFileTTF(font.c_str(), kFontSize))
        SDL_Log("[settings] could not load %s; using ImGui's built-in font",
                font.c_str());

    if (!ImGui_ImplSDL3_InitForSDLRenderer(g.window, g.renderer) ||
        !ImGui_ImplSDLRenderer3_Init(g.renderer)) {
        error_out = "could not attach ImGui to the settings window";
        teardown();
        return false;
    }

    g.spec     = spec;
    g.fields   = spec.fields;
    g.baseline = spec.fields;
    g.reported.clear();
    for (const Field& f : g.fields) g.reported.push_back(valueOf(f));
    collectTabs();

    g_open = true;
    return true;
}

void close()
{
    if (!g_open) return;

    // Copied out before teardown clears the spec: the callback is allowed to
    // ask whether the window is open, and by the time it runs it is not.
    std::function<void()> on_close = g.spec.on_close;
    g.spec = Spec();
    teardown();
    if (on_close) on_close();
}

bool handleEvent(const SDL_Event& e)
{
    if (!g_open) return false;

    const SDL_WindowID id = eventWindow(e);
    if (id != SDL_GetWindowID(g.window)) return false;

    ImGui::SetCurrentContext(g.ctx);
    ImGui_ImplSDL3_ProcessEvent(&e);

    // The title bar's close button is Cancel. A dialog dismissed by the window
    // manager has not agreed to anything.
    if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        if (g.spec.on_cancel) g.spec.on_cancel();
        g.closing = true;
    }
    return true;
}

void render()
{
    if (!g_open) return;

    ImGui::SetCurrentContext(g.ctx);
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    drawWindow();

    ImGui::Render();
    SDL_SetRenderDrawColor(g.renderer, 0x1c, 0x1e, 0x24, 0xff);
    SDL_RenderClear(g.renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g.renderer);
    SDL_RenderPresent(g.renderer);

    // After the frame, never during it: a button's callback must not destroy
    // the context it is being drawn in.
    if (g.closing) close();
}

} // namespace Settings
