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

#include "core/components/ComponentScheduler.h"
#include "core/components/Component.h"

namespace cc {

// ---- Start phase ----

void ComponentScheduler::addStart(Component *comp) {
    _startList.add(comp, 0); // Start has no execution order, always zero bucket
}

void ComponentScheduler::removeStart(Component *comp) {
    _startList.remove(comp);
}

void ComponentScheduler::invokeStart() {
    _invokingStart = true;
    _startList.forEach([](Component *comp) {
        comp->start();
        comp->_started = true;
    });
    _startList.clear();
    _invokingStart = false;
}

// ---- Update phase ----

void ComponentScheduler::addUpdate(Component *comp, int32_t executionOrder) {
    _updateList.add(comp, executionOrder);
}

void ComponentScheduler::removeUpdate(Component *comp) {
    _updateList.remove(comp);
}

void ComponentScheduler::invokeUpdate(float dt) {
    _invokingUpdate = true;
    _updateList.forEach([dt](Component *comp) {
        comp->update(dt);
    });
    _invokingUpdate = false;
}

// ---- Late update phase ----

void ComponentScheduler::addLateUpdate(Component *comp, int32_t executionOrder) {
    _lateUpdateList.add(comp, executionOrder);
}

void ComponentScheduler::removeLateUpdate(Component *comp) {
    _lateUpdateList.remove(comp);
}

void ComponentScheduler::invokeLateUpdate(float dt) {
    _invokingLateUpdate = true;
    _lateUpdateList.forEach([dt](Component *comp) {
        comp->lateUpdate(dt);
    });
    _invokingLateUpdate = false;
}

// ---- Lifecycle registration helpers ----

void ComponentScheduler::registerComponent(Component *comp) {
    bool registered = false;
    if (comp->hasStartMethod() && !comp->_started) {
        addStart(comp);
    }
    if (comp->hasUpdateMethod()) {
        addUpdate(comp); // TODO: support executionOrder per component
        registered = true;
    }
    if (comp->hasLateUpdateMethod()) {
        addLateUpdate(comp);
        registered = true;
    }
    comp->_registeredToScheduler = registered;
}

void ComponentScheduler::unregisterComponent(Component *comp) {
    removeStart(comp);
    removeUpdate(comp);
    removeLateUpdate(comp);
    comp->_registeredToScheduler = false;
}

// ---- Clear all ----

void ComponentScheduler::clear() {
    _startList.clear();
    _updateList.clear();
    _lateUpdateList.clear();
}

} // namespace cc
