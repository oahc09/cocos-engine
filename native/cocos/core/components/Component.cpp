/****************************************************************************
 Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/

#include "core/components/Component.h"
#include "core/scene-graph/Node.h"

namespace cc {

Component::~Component() = default;

void Component::setEnabled(bool value) {
    if (_enabled == value) return;
    _enabled = value;
    // Node activator will handle onEnable/onDisable and scheduler registration
    // This will be wired up in M3 (NodeActivator)
}

void Component::__preload() {}
void Component::onLoad() {}
void Component::start() {}
void Component::update(float dt) {}
void Component::lateUpdate(float dt) {}
void Component::onEnable() {}
void Component::onDisable() {}
void Component::onDestroy() {}

void Component::deserializeBinary(const uint8_t *data, uint32_t size) {
    // Default: no-op. Override in subclasses.
}

ccstd::vector<Asset *> Component::getAssetProperties() {
    return {};
}

void Component::schedule(const std::function<void(float)> &callback, float interval,
                          unsigned int repeat, float delay, bool paused) {
    // TODO: Wire to Director::getInstance()->getScheduler() in M5
}

void Component::unschedule(const std::function<void(float)> &callback) {
    // TODO: Wire to Director::getInstance()->getScheduler() in M5
}

bool Component::destroy() {
    if (!isValid()) return false;
    if (_node) {
        // TODO: _node->removeComponent(this) in M3
    }
    return Super::destroy();
}

void Component::destruct() {
    // Clean up references
    _node = nullptr;
    Super::destruct();
}

} // namespace cc
