/*
 Copyright (c) 2020-2023 Xiamen Yaji Software Co., Ltd.

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

import { IMat4Like, Mat4, Quat, Vec3 } from '../core/math';

declare const jsb: any;

// For optimize getPosition, getRotation, getScale
export const _tempFloatArray = new Float32Array(jsb.createExternalArrayBuffer(20 * 4));

export const fillMat4WithTempFloatArray = function fillMat4WithTempFloatArray (out: IMat4Like) {
    Mat4.set(out,
        _tempFloatArray[0], _tempFloatArray[1], _tempFloatArray[2], _tempFloatArray[3],
        _tempFloatArray[4], _tempFloatArray[5], _tempFloatArray[6], _tempFloatArray[7],
        _tempFloatArray[8], _tempFloatArray[9], _tempFloatArray[10], _tempFloatArray[11],
        _tempFloatArray[12], _tempFloatArray[13], _tempFloatArray[14], _tempFloatArray[15]
    );
};

export function resolveNodeVec3Args (
    val: Readonly<Vec3> | number,
    y: number | undefined,
    z: number | undefined,
    currentZ: number,
): { x: number; y: number; z: number } {
    if (y === undefined && z === undefined) {
        const vec = val as Readonly<Vec3>;
        return { x: vec.x, y: vec.y, z: vec.z };
    }

    if (z === undefined) {
        return { x: val as number, y: y as number, z: currentZ };
    }

    return { x: val as number, y: y as number, z };
}

export function resolveNodeRTSArgs (
    rot?: Quat | Vec3,
    pos?: Vec3,
    scale?: Vec3,
): {
    rotation: Quat | null;
    position: { x: number; y: number; z: number } | null;
    scale: { x: number; y: number; z: number } | null;
} {
    let rotation: Quat | null = null;
    if (rot) {
        if (rot instanceof Quat) {
            rotation = new Quat(rot.x, rot.y, rot.z, rot.w);
        } else {
            rotation = new Quat();
            Quat.fromEuler(rotation, rot.x, rot.y, rot.z);
        }
    }

    return {
        rotation,
        position: pos ? { x: pos.x, y: pos.y, z: pos.z } : null,
        scale: scale ? { x: scale.x, y: scale.y, z: scale.z } : null,
    };
}

export function resolveNodeQuatArgs (
    val: Readonly<Quat> | number,
    y: number | undefined,
    z: number | undefined,
    w: number | undefined,
    currentW: number,
): { x: number; y: number; z: number; w: number } {
    if (y === undefined && z === undefined && w === undefined) {
        const quat = val as Readonly<Quat>;
        return { x: quat.x, y: quat.y, z: quat.z, w: quat.w };
    }

    return {
        x: val as number,
        y: y as number,
        z: z as number,
        w: w === undefined ? currentW : w,
    };
}
