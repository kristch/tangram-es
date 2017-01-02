#include "labels.h"

#include "tangram.h"
#include "platform.h"
#include "gl/shaderProgram.h"
#include "gl/primitives.h"
#include "view/view.h"
#include "style/style.h"
#include "style/pointStyle.h"
#include "style/textStyle.h"
#include "tile/tile.h"
#include "tile/tileCache.h"
#include "labels/labelSet.h"
#include "labels/textLabel.h"
#include "marker/marker.h"
#include "labels/curvedLabel.h"

#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include "glm/gtx/rotate_vector.hpp"
#include "glm/gtx/norm.hpp"

namespace Tangram {

Labels::Labels()
    : m_needUpdate(false),
      m_lastZoom(0.0f) {}

Labels::~Labels() {}

// int Labels::LODDiscardFunc(float _maxZoom, float _zoom) {
//     return (int) MIN(floor(((log(-_zoom + (_maxZoom + 2)) / log(_maxZoom + 2) * (_maxZoom )) * 0.5)), MAX_LOD);
// }

void Labels::processLabelUpdate(const ViewState& viewState,
                                StyledMesh* mesh, Tile* tile,
                                const glm::mat4& mvp,
                                float dt, bool drawAll,
                                bool onlyTransitions, bool isProxy) {

    if (!mesh) { return; }
    auto labelMesh = dynamic_cast<const LabelSet*>(mesh);
    if (!labelMesh) { return; }

    for (auto& label : labelMesh->getLabels()) {
        if (!drawAll && label->state() == Label::State::dead) {
            continue;
        }

        Range transformRange;
        ScreenTransform transform { m_transforms, transformRange, true };

        if (!label->update(mvp, viewState, transform, drawAll)) {
            // skip dead labels
            continue;
        }

        if (onlyTransitions) {
            if (label->occludedLastFrame()) { label->occlude(); }

            if (label->visibleState() || !label->canOcclude()) {
                m_needUpdate |= label->evalState(dt);
                label->addVerticesToMesh(transform, viewState.viewportSize);
            }
        } else if (label->canOcclude()) {
            m_labels.emplace_back(label.get(), tile, isProxy);
            m_labels.back().transform = transformRange;
        } else {
            m_needUpdate |= label->evalState(dt);
            label->addVerticesToMesh(transform, viewState.viewportSize);
        }
        if (label->selectionColor()) {
            m_selectionLabels.emplace_back(label.get(), tile, isProxy);
        }
    }
}


std::pair<Label*, Tile*> Labels::getLabel(uint32_t _selectionColor) {

    for (auto& entry : m_selectionLabels) {

        if (entry.label->visibleState() &&
            entry.label->selectionColor() == _selectionColor) {

            return { entry.label, entry.tile };
        }
    }
    return {nullptr, nullptr};
}


void Labels::updateLabels(const ViewState& _viewState, float _dt,
                          const std::vector<std::unique_ptr<Style>>& _styles,
                          const std::vector<std::shared_ptr<Tile>>& _tiles,
                          const std::vector<std::unique_ptr<Marker>>& _markers,
                          bool _onlyTransitions) {

    if (!_onlyTransitions) { m_labels.clear(); }

    m_selectionLabels.clear();

    m_needUpdate = false;

    // int lodDiscard = LODDiscardFunc(View::s_maxZoom, _view.getZoom());

    bool drawAllLabels = Tangram::getDebugFlag(DebugFlags::draw_all_labels);

    for (const auto& tile : _tiles) {

        //LOG("tile: %d/%d z:%d,%d", tile->getID().x, tile->getID().y, tile->getID().z, tile->getID().s);

        // discard based on level of detail
        // if ((zoom - tile->getID().z) > lodDiscard) {
        //     continue;
        // }

        bool proxyTile = tile->isProxy();

        glm::mat4 mvp = tile->mvp();

        for (const auto& style : _styles) {
            const auto& mesh = tile->getMesh(*style);
            processLabelUpdate(_viewState, mesh.get(), tile.get(), mvp,
                               _dt, drawAllLabels, _onlyTransitions, proxyTile);
        }
    }

    for (const auto& marker : _markers) {
        for (const auto& style : _styles) {

            if (marker->styleId() != style->getID()) { continue; }

            const auto& mesh = marker->mesh();

            processLabelUpdate(_viewState, mesh, nullptr,
                               marker->modelViewProjectionMatrix(),
                               _dt, drawAllLabels, _onlyTransitions, false);
        }
    }
}

void Labels::skipTransitions(const std::vector<const Style*>& _styles, Tile& _tile, Tile& _proxy) const {

    for (const auto& style : _styles) {

        auto* mesh0 = dynamic_cast<const LabelSet*>(_tile.getMesh(*style).get());
        if (!mesh0) { continue; }

        auto* mesh1 = dynamic_cast<const LabelSet*>(_proxy.getMesh(*style).get());
        if (!mesh1) { continue; }

        for (auto& l0 : mesh0->getLabels()) {
            if (!l0->canOcclude()) { continue; }
            if (l0->state() != Label::State::none) { continue; }

            for (auto& l1 : mesh1->getLabels()) {
                if (!l1->visibleState()) { continue; }
                if (!l1->canOcclude()) { continue;}

                // Using repeat group to also handle labels with dynamic style properties
                if (l0->options().repeatGroup != l1->options().repeatGroup) { continue; }
                // if (l0->hash() != l1->hash()) { continue; }

                float d2 = glm::distance2(l0->screenCenter(), l1->screenCenter());

                // The new label lies within the circle defined by the bbox of l0
                if (sqrt(d2) < std::max(l0->dimension().x, l0->dimension().y)) {
                    l0->skipTransitions();
                }
            }
        }
    }
}

std::shared_ptr<Tile> findProxy(int32_t _sourceID, const TileID& _proxyID,
                                const std::vector<std::shared_ptr<Tile>>& _tiles,
                                TileCache& _cache) {

    auto proxy = _cache.contains(_sourceID, _proxyID);
    if (proxy) { return proxy; }

    for (auto& tile : _tiles) {
        if (tile->getID() == _proxyID && tile->sourceID() == _sourceID) {
            return tile;
        }
    }
    return nullptr;
}

void Labels::skipTransitions(const std::vector<std::unique_ptr<Style>>& _styles,
                             const std::vector<std::shared_ptr<Tile>>& _tiles,
                             TileCache& _cache, float _currentZoom) const {

    std::vector<const Style*> styles;

    for (const auto& style : _styles) {
        if (dynamic_cast<const TextStyle*>(style.get()) ||
            dynamic_cast<const PointStyle*>(style.get())) {
            styles.push_back(style.get());
        }
    }

    for (const auto& tile : _tiles) {
        TileID tileID = tile->getID();
        std::shared_ptr<Tile> proxy;

        if (m_lastZoom < _currentZoom) {
            // zooming in, add the one cached parent tile
            proxy = findProxy(tile->sourceID(), tileID.getParent(), _tiles, _cache);
            if (proxy) { skipTransitions(styles, *tile, *proxy); }
        } else {
            // zooming out, add the 4 cached children tiles
            proxy = findProxy(tile->sourceID(), tileID.getChild(0), _tiles, _cache);
            if (proxy) { skipTransitions(styles, *tile, *proxy); }

            proxy = findProxy(tile->sourceID(), tileID.getChild(1), _tiles, _cache);
            if (proxy) { skipTransitions(styles, *tile, *proxy); }

            proxy = findProxy(tile->sourceID(), tileID.getChild(2), _tiles, _cache);
            if (proxy) { skipTransitions(styles, *tile, *proxy); }

            proxy = findProxy(tile->sourceID(), tileID.getChild(3), _tiles, _cache);
            if (proxy) { skipTransitions(styles, *tile, *proxy); }
        }
    }
}

bool Labels::labelComparator(const LabelEntry& _a, const LabelEntry& _b) {
    if (_a.proxy != _b.proxy) {
        return _b.proxy;
    }
    if (_a.priority != _b.priority) {
        return _a.priority < _b.priority;
    }
    if (!_a.tile || !_b.tile) {
        return (bool)_a.tile;
    }
    if (_a.tile->getID().z != _b.tile->getID().z) {
        return _a.tile->getID().z > _b.tile->getID().z;
    }

    auto l1 = _a.label;
    auto l2 = _b.label;

    // Note: This causes non-deterministic placement, i.e. depending on
    // navigation history.
    if (l1->occludedLastFrame() != l2->occludedLastFrame()) {
        return l2->occludedLastFrame();
    }
    // This prefers labels within screen over out_of_screen.
    // Important for repeat groups!
    if (l1->visibleState() != l2->visibleState()) {
        return l1->visibleState();
    }

    // if (l1->options().repeatGroup != l2->options().repeatGroup) {
    //     return l1->options().repeatGroup < l2->options().repeatGroup;
    // }

    if (l1->type() == Label::Type::line && l2->type() == Label::Type::line) {
        // Prefer the label with longer line segment as it has a chance
        return l1->worldLineLength2() > l2->worldLineLength2();
    }

    if (l1->hash() != l2->hash()) {
        return l1->hash() < l2->hash();
    }

    if (l1->type() == Label::Type::curved &&
        l2->type() == Label::Type::curved) {
        return (static_cast<const CurvedLabel*>(l1)->candidatePriority() >
                static_cast<const CurvedLabel*>(l2)->candidatePriority());
    }

    return l1 < l2;
}

void Labels::sortLabels() {
    // Use stable sort so that relative ordering of markers is preserved.
    std::stable_sort(m_labels.begin(), m_labels.end(), Labels::labelComparator);
}

void Labels::handleOcclusions(const ViewState& _viewState) {

    //glm::vec2 screenSize = glm::vec2(_view.getWidth(), _view.getHeight());
    //glm::vec2 screenSize = _viewState.viewportSize;
    //float dz = _view.getZoom() - std::floor(_view.getZoom());

    m_isect2d.clear();
    m_repeatGroups.clear();

    for (auto& entry : m_labels){
        auto* l = entry.label;

        // Parent must have been processed earlier so at this point its
        // occlusion and anchor position is determined for the current frame.
        if (l->parent()) {
            if (l->parent()->isOccluded()) {
                l->occlude();
                continue;
            }
        }

        ScreenTransform transform { m_transforms, entry.transform };

        l->obbs(transform, m_obbs, entry.obbs);

        // Skip label if another label of this repeatGroup is
        // within repeatDistance.
        if (l->options().repeatDistance > 0.f && withinRepeatDistance(l)) {
            l->occlude();
        }

        int anchorIndex = l->anchorIndex();

        // For each anchor
        do {
            if (l->isOccluded()) {
                // Update BBox for anchor fallback
                l->obbs(transform, m_obbs, entry.obbs, false);

                if (anchorIndex == l->anchorIndex()) {
                    // Reached first anchor again
                    break;
                }
            }

            l->occlude(false);

            // Skip label if it intersects with a previous label.
            for (int i = entry.obbs.start; i < entry.obbs.end(); i++) {
                auto& obb = m_obbs[i];

                m_isect2d.intersect(obb.getExtent(), [&](auto& a, auto& b) {
                        size_t other = reinterpret_cast<size_t>(b.m_userData);

                        if (l->parent()) {
                            // Ignore intersection with parent label
                            // TODO: optimize
                            auto it = std::lower_bound(std::begin(m_labels), std::end(m_labels), int(other),
                                                       [](auto& p, int other) { return other > p.obbs.start; });

                            if (it != std::end(m_labels)) {
                                if (l->parent() == it->label) {
                                    return true;
                                }
                            }
                        }

                        if (intersect(obb, m_obbs[other])) {
                            l->occlude();
                            // Drop current label
                            return false;
                        }
                        // Continue
                        return true;

                    }, false);

                if (l->isOccluded()) { break; }
            }
        } while (l->isOccluded() && l->nextAnchor());

        if (l->isOccluded()) {
            if (l->parent() && l->options().required) {
                l->parent()->occlude();
            }
        } else {
            // Insert into ISect2D grid
            for (int i = entry.obbs.start; i < entry.obbs.end(); i++) {
                auto aabb = m_obbs[i].getExtent();
                aabb.m_userData = reinterpret_cast<void*>(i);
                m_isect2d.insert(aabb);
            }

            if (l->options().repeatDistance > 0.f) {
                m_repeatGroups[l->options().repeatGroup].push_back(l);
            }
        }
    }
}

bool Labels::withinRepeatDistance(Label *_label) {
    float threshold2 = pow(_label->options().repeatDistance, 2);

    auto it = m_repeatGroups.find(_label->options().repeatGroup);
    if (it != m_repeatGroups.end()) {
        for (auto* ll : it->second) {
            float d2 = glm::distance2(_label->screenCenter(), ll->screenCenter());
            if (d2 < threshold2) {
                return true;
            }
        }
    }
    return false;
}

void Labels::updateLabelSet(const ViewState& _viewState, float _dt,
                            const std::vector<std::unique_ptr<Style>>& _styles,
                            const std::vector<std::shared_ptr<Tile>>& _tiles,
                            const std::vector<std::unique_ptr<Marker>>& _markers,
                            TileCache& _cache) {

    m_transforms.clear();
    m_obbs.clear();

    /// Collect and update labels from visible tiles
    updateLabels(_viewState, _dt, _styles, _tiles, _markers, false);

    sortLabels();

    /// Mark labels to skip transitions

    if (int(m_lastZoom) != int(_viewState.zoom)) {
        skipTransitions(_styles, _tiles, _cache, _viewState.zoom);
        m_lastZoom = _viewState.zoom;
    }

    m_isect2d.resize({_viewState.viewportSize.x / 256, _viewState.viewportSize.y / 256},
                     {_viewState.viewportSize.x, _viewState.viewportSize.y});

    handleOcclusions(_viewState);

    // Update label meshes
    for (auto& entry : m_labels) {
        ScreenTransform transform { m_transforms, entry.transform };

        m_needUpdate |= entry.label->evalState(_dt);
        entry.label->addVerticesToMesh(transform, _viewState.viewportSize);
    }
}

void Labels::drawDebug(RenderState& rs, const View& _view) {

    if (!Tangram::getDebugFlag(Tangram::DebugFlags::labels)) {
        return;
    }

    for (auto& entry : m_labels) {
        auto* label = entry.label;

        if (label->type() == Label::Type::debug) { continue; }

        glm::vec2 sp = label->screenCenter();

        // draw bounding box
        switch (label->state()) {
        case Label::State::sleep:
            Primitives::setColor(rs, 0xffffff);
            break;
        case Label::State::visible:
            Primitives::setColor(rs, 0x000000);
            break;
        case Label::State::none:
            Primitives::setColor(rs, 0x0000ff);
            break;
        case Label::State::dead:
            Primitives::setColor(rs, 0xff00ff);
            break;
        case Label::State::fading_in:
            Primitives::setColor(rs, 0xffff00);
            break;
        case Label::State::fading_out:
            Primitives::setColor(rs, 0xff0000);
            break;
        default:
            Primitives::setColor(rs, 0x999999);
        }

#if DEBUG_OCCLUSION
        if (label->isOccluded()) {
            Primitives::setColor(rs, 0xff0000);
            if (label->occludedLastFrame()) {
                Primitives::setColor(rs, 0xffff00);
            }
        } else if (label->occludedLastFrame()) {
            Primitives::setColor(rs, 0x00ff00);
        } else {
            Primitives::setColor(rs, 0x000000);
        }
#endif

        for (int i = entry.obbs.start; i < entry.obbs.end(); i++) {

            Primitives::drawPoly(rs, &(m_obbs[i].getQuad())[0], 4);
        }

        if (label->parent()) {
            Primitives::setColor(rs, 0xff0000);
            Primitives::drawLine(rs, sp, label->parent()->screenCenter());
        }

        if (label->type() == Label::Type::curved) {
            //for (int i = entry.transform.start; i < entry.transform.end()-2; i++) {
            for (int i = entry.transform.start; i < entry.transform.end()-1; i++) {
                if (i % 2 == 0) {
                    Primitives::setColor(rs, 0xff0000);
                } else {
                    Primitives::setColor(rs, 0x0000ff);

                }
                Primitives::drawLine(rs, glm::vec2(m_transforms.points[i]),
                                     glm::vec2(m_transforms.points[i+1]));
            }
        }
        // draw offset
        glm::vec2 rot = glm::vec2{};//label->screenTransform().rotation;
        glm::vec2 offset = label->options().offset;
        if (label->parent()) { offset += label->parent()->options().offset; }
        offset = rotateBy(offset, rot);

        Primitives::setColor(rs, 0x000000);
        Primitives::drawLine(rs, sp, sp - glm::vec2(offset.x, -offset.y));

#if 0
        if (label->options().repeatGroup != 0 && label->state() == Label::State::visible) {
            size_t seed = 0;
            hash_combine(seed, label->options().repeatGroup);
            float repeatDistance = label->options().repeatDistance;

            Primitives::setColor(seed);
            Primitives::drawLine(label->center(),
                                 glm::vec2(repeatDistance, 0.f) + label->center());

            float off = M_PI / 6.f;
            for (float pad = 0.f; pad < M_PI * 2.f; pad += off) {
                glm::vec2 p0 = glm::vec2(cos(pad), sin(pad)) * repeatDistance
                    + label->center();
                glm::vec2 p1 = glm::vec2(cos(pad + off), sin(pad + off)) * repeatDistance
                    + label->center();
                Primitives::drawLine(p0, p1);
            }
        }
#endif
    }

    glm::vec2 split(_view.getWidth() / 256, _view.getHeight() / 256);
    glm::vec2 res(_view.getWidth(), _view.getHeight());
    const short xpad = short(ceilf(res.x / split.x));
    const short ypad = short(ceilf(res.y / split.y));

    Primitives::setColor(rs, 0x7ef586);
    short x = 0, y = 0;
    for (int j = 0; j < split.y; ++j) {
        for (int i = 0; i < split.x; ++i) {
            AABB cell(x, y, x + xpad, y + ypad);
            Primitives::drawRect(rs, {x, y}, {x + xpad, y + ypad});
            x += xpad;
            if (x >= res.x) {
                x = 0;
                y += ypad;
            }
        }
    }
}

}
