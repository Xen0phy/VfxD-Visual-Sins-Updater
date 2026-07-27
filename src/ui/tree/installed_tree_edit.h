#pragma once
#include "core/merge.h" // nlohmann::ordered_json
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// The tree-editing subsystem (right-click-to-edit, category rename,
// delete, create-category, and effect/category drag-and-drop reordering),
// split out of addon.cpp. Covers all six state
// machines now: the foundation below, per-category ops -- rename,
// create, move -- and per-effect ops -- edit, delete, move.
// RenderCategoryTree/RenderInstalledEffects (still in
// addon.cpp) reach this subsystem only through the
// accessor API declared throughout this header, never through its
// statics directly.
// ---------------------------------------------------------------------------

// Splits `text` into trimmed, non-empty lines. Used to turn the guids text
// box back into a guid list (effect editor).
std::vector<std::string> SplitLines(const std::string& text);

// True if `prefix` is `path`'s first N elements (N = prefix.size()),
// including the case where they're equal. Used to tell whether an
// in-progress edit lives at or underneath a category that's about to be
// collapsed -- see the "cancel on collapse" checks in RenderCategoryTree.
bool PathHasPrefix(const std::vector<int>& path, const std::vector<int>& prefix);

// Read-only category lookup by index path, starting from `root` (a sin
// file's top-level json, which has its own "categories" array exactly like
// any other category node). Each element of `path` is that level's
// position within its parent's "categories" array at the moment the path
// was captured during a tree walk (see `pathSoFar` in RenderCategoryTree)
// -- category identity is index-based, so same-named sibling categories
// can never collide here. Returns nullptr if any segment is out of range
// for the array at that point -- callers treat that as "the tree changed
// since editing started, don't guess."
nlohmann::ordered_json* FindCategoryByPath(nlohmann::ordered_json& root, const std::vector<int>& path);

// Same walk as FindCategoryByPath, but collects each step's "name" field
// instead of returning a pointer, joined the same way JoinPath (see
// installed_tree_overlay.h) formats a typed destination
// ("Combat / Downstate") -- used only for display (result/error messages),
// never for identity. Meant to be called right after a FindCategoryByPath
// lookup on the same path has already succeeded; if the tree were to
// change in between, this just resolves as much as it still can rather
// than asserting.
std::string JoinCategoryPathNames(const nlohmann::ordered_json& root, const std::vector<int>& path);

// Shown until the next edit action of *any* kind -- effect edit, category
// rename, delete, create category, or move -- success or failure. Shared
// across all six edit state machines (this couldn't stay an
// addon.cpp-local static, since RenderInstalledEffects displays it and
// every state machine sets it, not just three of the six). Every
// Begin*/Cancel*/Apply* function calls these instead of touching a
// static of the same name.
void SetEditResultMessage(const std::string& message);
void ClearEditResultMessage();
const std::string& GetEditResultMessage();

// True if any one of the six state machines below is currently active,
// addon-wide -- used to grey out/disable starting a different
// edit/rename/delete/create/move while another is already open. Lives
// here (not addon.cpp) since it needs state from all six state machines.
bool AnyEditInFlight();

// ---------------------------------------------------------------------------
// Category rename. A deliberately much smaller sibling of the
// effect editor (below) -- only the category's own "name"
// field is editable; moving a category (changing its parent) is handled
// separately by the move-category API further down, and reparenting
// (moving to a *different* parent) isn't offered at all -- see
// ApplyPendingCategoryMove's own comment for why.
// ---------------------------------------------------------------------------

// Populates the rename buffer and marks it active. `path` is this
// category's own identity, root -> ... -> this category, inclusive.
void BeginCategoryEdit(const std::string& sinName, const std::vector<int>& path, const std::string& currentName);
void CancelCategoryEdit();

// Draws the rename widget for whichever category is currently being
// renamed. Only meant to be called from inside that one category's
// TreeNode (see IsCategoryBeingRenamed).
void RenderCategoryEditor();

// Applies a previously-recorded rename (queued by RenderCategoryEditor's
// Save button) to the in-memory json and writes it to disk. Safe to call
// unconditionally every frame -- no-ops if nothing is pending, same as
// every other Apply* function here.
void ApplyPendingCategoryRename();

// True if the category at exactly this path (in this sin file) is the one
// currently being renamed -- used by RenderCategoryTree to decide whether
// to draw the rename editor inline at this node.
bool IsCategoryBeingRenamed(const std::string& sinName, const std::vector<int>& path);

// True if the category currently being renamed (if any, in this sin file)
// is at or underneath `path` -- i.e. whether collapsing the node at `path`
// would hide it. Used to cancel an in-progress rename when its owning
// node collapses.
bool IsCategoryRenameUnderPath(const std::string& sinName, const std::vector<int>& path);

// True if a category rename is active anywhere (any sin file). Feeds
// AnyEditInFlight, which stays in
// addon.cpp and calls this instead of reading a local `s_categoryEdit`
// static directly.
bool IsCategoryRenameActive();

// ---------------------------------------------------------------------------
// Create category. Rendered inside the parent category's
// TreeNode (same idea as RenderCategoryEditor above for rename), so this
// DOES get cancelled on collapse, same reasoning as category rename.
// ---------------------------------------------------------------------------

// `parentPath` is where the new category goes; empty means this sin
// file's top level.
void BeginCreateCategory(const std::string& sinName, const std::vector<int>& parentPath);
void CancelCreateCategory();

// Draws the inline "new subcategory" prompt for whichever category is
// currently the target parent (see IsCreatingCategoryAt).
void RenderCreateCategoryEditor();

// Applies a previously-recorded category creation to the in-memory json
// and writes it to disk. Safe to call unconditionally; no-ops if nothing
// is pending.
void ApplyPendingCreateCategory();

// True if a new-category prompt targeting exactly this parent path (in
// this sin file) is currently open.
bool IsCreatingCategoryAt(const std::string& sinName, const std::vector<int>& parentPath);

// True if the create-category prompt currently open (if any, in this sin
// file) targets a parent at or underneath `path`. Used to cancel it when
// its owning node collapses, same shape as IsCategoryRenameUnderPath.
bool IsCategoryCreateUnderPath(const std::string& sinName, const std::vector<int>& path);

// True if a create-category prompt is open anywhere (any sin file). Feeds
// AnyEditInFlight -- see IsCategoryRenameActive's comment above.
bool IsCreateCategoryActive();

// ---------------------------------------------------------------------------
// Move category -- reorder-only drag-and-drop. A category only
// ever lands back among its own *current* siblings, at a new position --
// dragging it into a *different* parent (which would carry every effect
// and subcategory underneath it along for the ride) isn't offered, same
// reasoning category rename's comment above gives for why rename doesn't
// offer reparenting either.
// ---------------------------------------------------------------------------

// Records that the category at `path` (in `sinName`) is the one currently
// being dragged, and hands ImGui the drag payload -- call from inside
// ImGui::BeginDragDropSource(), same as before this moved. The category's
// own identity is `path` itself (root -> ... -> this category, inclusive);
// path.back() is its index within its parent's "categories" array.
void BeginCategoryDrag(const std::string& sinName, const std::vector<int>& path);

// The sin name / category path most recently recorded by BeginCategoryDrag
// this frame -- a drop target reads these to decide whether (and how) to
// queue a move. An empty path means nothing is currently being dragged.
const std::string& GetCategoryDragSinName();
const std::vector<int>& GetCategoryDragPath();

// Records a category reorder to be applied once the whole tree has
// finished rendering this frame. `originalPath` is the dragged category's
// identity at drag time (see BeginCategoryDrag); `destinationIndex` is -1
// for "append" (dropped on the shared parent's own row) or the sibling
// index -- within that same parent's "categories" array, as captured at
// drop time, before any erase has run -- that the category should end up
// immediately above (dropped on that sibling's own row). See
// ApplyPendingCategoryMove for the full write-up, including how the
// erase-shift is reconciled.
void QueueCategoryMove(const std::string& sinName, const std::vector<int>& originalPath, int destinationIndex);

// Applies a previously-queued category reorder to the in-memory json and
// writes it to disk. Safe to call unconditionally every frame; no-ops if
// nothing is pending.
void ApplyPendingCategoryMove();

// ---------------------------------------------------------------------------
// Per-effect ops: the effect editor, delete confirmation (shared
// with category delete, via the isCategory flag -- see BeginDeleteConfirm),
// and effect drag-and-drop move. RenderCategoryTree/RenderInstalledEffects
// (still in addon.cpp) call through the API below instead of
// touching this subsystem's statics directly -- same shape as
// installed_tree_store's API for the same reason.
//
// SetEditResultMessage/ClearEditResultMessage/GetEditResultMessage are
// declared above, not here -- s_editResultMessage turned out to be shared
// by all six edit/rename/delete/create/move state machines, not scoped
// to the effect editor alone, so it's declared once, up top.
// ---------------------------------------------------------------------------

// --- Effect editor -----------------------------------------------------

void BeginEdit(const std::string& sinName, const std::vector<int>& path,
                int index, const nlohmann::ordered_json& effect);
void CancelEdit();
void RenderEffectEditor();
void ApplyPendingEdit();

// True if the effect at exactly (sinName, path, index) is the one currently
// being edited -- replaces RenderCategoryTree's direct `s_edit.active &&
// s_edit.sinName == ... && s_edit.originalPath == ... && s_edit.originalIndex
// == ...` reads (the "(editing)" label and the isEditingThis/
// isEditingThisHidden checks).
bool IsEffectBeingEdited(const std::string& sinName, const std::vector<int>& path, int index);

// True if an edit is active anywhere at or underneath `path` for `sinName`
// -- used only for the "cancel on collapse" checks (PathHasPrefix against
// the in-progress edit's own path), never for identity against one exact
// effect the way IsEffectBeingEdited is.
bool IsEffectEditUnderPath(const std::string& sinName, const std::vector<int>& path);

// True if an effect edit is active anywhere, addon-wide -- one of the four
// checks AnyEditInFlight (still in addon.cpp) ORs together.
bool IsEffectEditActive();

// --- Delete confirm (effects and categories -- see BeginDeleteConfirm) -----

void BeginDeleteConfirm(const std::string& sinName, const std::vector<int>& path, int index,
                         bool isCategory, const std::string& displayName);
void CancelDeleteConfirm();
void RenderDeleteConfirm();
void ApplyPendingDelete();

// True if the pending delete confirmation is for exactly this category
// (`path` is the category's own identity, inclusive) -- replaces
// RenderCategoryTree's direct `s_deleteConfirm.active &&
// s_deleteConfirm.isCategory && ...` read.
bool IsDeletingThisCategory(const std::string& sinName, const std::vector<int>& path);

// True if the pending delete confirmation is for exactly this effect
// (`path` is its containing category, `index` its position within it) --
// replaces the equivalent direct `s_deleteConfirm` read for effects.
bool IsDeletingThisEffect(const std::string& sinName, const std::vector<int>& path, int index);

// True if a delete confirmation (effect or category) is active anywhere,
// addon-wide -- another of AnyEditInFlight's four checks.
bool IsDeleteConfirmActive();

// True if the pending delete confirmation (effect or category, if any, in
// this sin file) is at or underneath `path` -- i.e. whether hiding the
// node at `path` would hide it too. For a category delete,
// s_deleteConfirm's own path is that category's identity, inclusive, so
// this is a direct PathHasPrefix check; for an effect delete it's the
// containing category's identity, so hiding that category hides the
// effect (and its pending delete confirm) the same way. Unlike the other
// three UnderPath checks, this is NOT used by the "collapsed" cancel-on-
// collapse branch -- the delete confirm's own row (and its Cancel button)
// stays visible whether or not its owning node is open, so collapsing
// never needs to cancel it. It IS needed by the tree-search hide paths,
// where the whole row -- Cancel button included -- disappears entirely,
// unlike a mere collapse.
bool IsDeleteConfirmUnderPath(const std::string& sinName, const std::vector<int>& path);

// --- Effect move (drag-and-drop) -------------------------------------------

// Payload marker for ImGui::SetDragDropPayload/AcceptDragDropPayload on an
// effect drag -- the bytes themselves are just a type marker, never read
// back; the real source info travels via GetEffectDragPayload() below (see
// EffectDragPayload's own comment, carried over unchanged from addon.cpp).
inline constexpr int kEffectDragMarker = 1;

// Mirrors addon.cpp's old EffectDragPayload exactly. Exposed as a struct
// (rather than one getter per field) because RenderCategoryTree's drop-
// target logic reads several fields together for same-category/no-op
// comparisons -- splitting that into individual accessors would just be
// more surface for the same coupling.
struct EffectDragPayload
{
    std::string      sinName;
    std::vector<int> originalPath; // see EditState's originalPath comment (installed_tree_edit.cpp)
    std::string      effectName;
    int              originalIndex = -1;
};

// Records the current drag payload -- called every frame the drag is held,
// from RenderCategoryTree's BeginDragDropSource block, same as addon.cpp
// used to set s_dragPayload's fields directly.
void BeginEffectDrag(const std::string& sinName, const std::vector<int>& path,
                      const std::string& effectName, int index);

// Only meaningful while a drag is in progress (i.e. from inside a
// BeginDragDropTarget block that just accepted "VFXD_EFFECT").
const EffectDragPayload& GetEffectDragPayload();

// Mirrors addon.cpp's old EffectMoveJob exactly.
struct EffectMoveJob
{
    std::string      sinName;
    std::vector<int> originalPath;
    std::string      effectName;
    int              originalIndex = -1;
    std::vector<int> destinationPath;
    int              destinationIndex = -1;
};

// Records a move to apply once the whole tree has finished rendering this
// frame -- called from both of RenderCategoryTree's effect drop targets
// (the category-row "append" target and the specific-effect-row "insert
// above" target).
void QueueEffectMove(EffectMoveJob job);

void ApplyPendingMove();
