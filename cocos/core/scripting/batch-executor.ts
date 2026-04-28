/*
 Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

 https://www.cocos.com/

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
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
*/

import { getClassAttrs, DELIMETER } from '../data/utils/attribute';
import { legacyCC } from '../global-exports';

type ScriptBridgeMethod = (...args: unknown[]) => unknown;

interface SerializableConstructor {
    __values__?: string[];
}

const scriptBridgeInstances = new Map<number, unknown>();
const globalScriptBridge = globalThis as typeof globalThis & {
    __scriptBridgeBatchCall?: (compIds: number[], method: string, dt?: number) => void;
    __scriptBridgeCollectAssetRefs?: (compId: number) => unknown[];
};

export function registerScriptInstance (jsComp: unknown, compId: number, _className?: string): void {
    registerScriptBridgeInstance(compId, jsComp);
}

export function registerScriptBridgeInstance (compId: number, jsComp: unknown): void {
    scriptBridgeInstances.set(compId, jsComp);
}

export function unregisterScriptInstance (compId: number): void {
    unregisterScriptBridgeInstance(compId);
}

export function unregisterScriptBridgeInstance (compId: number): void {
    scriptBridgeInstances.delete(compId);
}

export function installScriptBridgeHelpers (globalJsb: Record<string, unknown>): void {
    globalScriptBridge.__scriptBridgeBatchCall = scriptBridgeBatchCall;
    globalScriptBridge.__scriptBridgeCollectAssetRefs = scriptBridgeCollectAssetRefs;
    globalJsb.__scriptBridgeBatchCall = scriptBridgeBatchCall;
    globalJsb.__scriptBridgeCollectAssetRefs = scriptBridgeCollectAssetRefs;
}

export function installScriptBridgeBatchCall (globalJsb: Record<string, unknown>): void {
    installScriptBridgeHelpers(globalJsb);
}

function scriptBridgeBatchCall (compIds: number[], method: string, dt?: number): void {
    if (!Array.isArray(compIds) || typeof method !== 'string' || method.length === 0) {
        return;
    }

    const hasDt = typeof dt === 'number';
    for (let i = 0; i < compIds.length; ++i) {
        const jsComp = scriptBridgeInstances.get(compIds[i]) as Record<string, unknown> | undefined;
        if (!jsComp) {
            continue;
        }

        const lifecycleMethod = jsComp[method];
        if (typeof lifecycleMethod !== 'function') {
            continue;
        }

        if (hasDt) {
            (lifecycleMethod as ScriptBridgeMethod).call(jsComp, dt);
        } else {
            (lifecycleMethod as ScriptBridgeMethod).call(jsComp);
        }
    }
}

function scriptBridgeCollectAssetRefs (compId: number): unknown[] {
    const jsComp = scriptBridgeInstances.get(compId);
    if (!jsComp || typeof jsComp !== 'object') {
        return [];
    }

    const assets: unknown[] = [];
    const collectedAssets = new Set<unknown>();
    const visitedObjects = new Set<object>();
    collectAssetRefs(jsComp, assets, collectedAssets, visitedObjects);
    return assets;
}

function collectAssetRefs (
    value: unknown,
    out: unknown[],
    collectedAssets: Set<unknown>,
    visitedObjects: Set<object>,
): void {
    if (!value || typeof value !== 'object') {
        return;
    }

    if (isAsset(value)) {
        if (!collectedAssets.has(value)) {
            collectedAssets.add(value);
            out.push(value);
        }
        return;
    }

    const target = value as object;
    if (visitedObjects.has(target)) {
        return;
    }
    visitedObjects.add(target);

    if (Array.isArray(value)) {
        for (let i = 0; i < value.length; ++i) {
            collectAssetRefs(value[i], out, collectedAssets, visitedObjects);
        }
        return;
    }

    if (isBinaryData(value)) {
        return;
    }

    const serializableProps = getSerializableProps(value);
    if (serializableProps) {
        for (let i = 0; i < serializableProps.length; ++i) {
            collectAssetRefs((value as Record<string, unknown>)[serializableProps[i]], out, collectedAssets, visitedObjects);
        }
        return;
    }

    if (!isPlainObject(value)) {
        return;
    }

    const keys = Object.keys(value);
    for (let i = 0; i < keys.length; ++i) {
        collectAssetRefs((value as Record<string, unknown>)[keys[i]], out, collectedAssets, visitedObjects);
    }
}

function getSerializableProps (value: object): string[] | null {
    const ctor = (value as { constructor?: SerializableConstructor }).constructor;
    const values = ctor?.__values__;
    if (!Array.isArray(values) || values.length === 0) {
        return null;
    }

    const attrs = getClassAttrs(ctor);
    const serializableProps: string[] = [];
    for (let i = 0; i < values.length; ++i) {
        const prop = values[i];
        if (attrs[`${prop}${DELIMETER}serializable`] === false) {
            continue;
        }
        serializableProps.push(prop);
    }
    return serializableProps;
}

function isAsset (value: unknown): boolean {
    const jsbAsset = globalThis.jsb?.Asset as (new (...args: never[]) => object) | undefined;
    if (jsbAsset && value instanceof jsbAsset) {
        return true;
    }

    const assetCtor = legacyCC.Asset as (new (...args: never[]) => object) | undefined;
    return !!assetCtor && value instanceof assetCtor;
}

function isBinaryData (value: object): boolean {
    return value instanceof ArrayBuffer
        || ArrayBuffer.isView(value);
}

function isPlainObject (value: object): boolean {
    const prototype = Object.getPrototypeOf(value);
    return prototype === Object.prototype || prototype === null;
}
