import { Vec3 } from '../../cocos/core/math';

(globalThis as typeof globalThis & { jsb?: unknown }).jsb = {
    createExternalArrayBuffer: (byteLength: number) => new ArrayBuffer(byteLength),
};

// eslint-disable-next-line @typescript-eslint/no-var-requires
const { resolveNodeVec3Args } = require('../../cocos/scene-graph/utils.jsb') as typeof import('../../cocos/scene-graph/utils.jsb');
const { resolveNodeRTSArgs } = require('../../cocos/scene-graph/utils.jsb') as typeof import('../../cocos/scene-graph/utils.jsb');
const { resolveNodeQuatArgs } = require('../../cocos/scene-graph/utils.jsb') as typeof import('../../cocos/scene-graph/utils.jsb');

describe('scene-graph utils.jsb', () => {
    test('resolveNodeVec3Args handles Vec3 input', () => {
        const result = resolveNodeVec3Args(new Vec3(1, 2, 3), undefined, undefined, 9);
        expect(result).toEqual({ x: 1, y: 2, z: 3 });
    });

    test('resolveNodeVec3Args preserves current z for 2D numeric input', () => {
        const result = resolveNodeVec3Args(4, 5, undefined, 9);
        expect(result).toEqual({ x: 4, y: 5, z: 9 });
    });

    test('resolveNodeVec3Args handles full numeric input', () => {
        const result = resolveNodeVec3Args(6, 7, 8, 9);
        expect(result).toEqual({ x: 6, y: 7, z: 8 });
    });

    test('resolveNodeRTSArgs converts Euler input to quaternion payload', () => {
        const result = resolveNodeRTSArgs(new Vec3(0, 90, 0), new Vec3(1, 2, 3), new Vec3(4, 5, 6));
        expect(result.rotation).not.toBeNull();
        expect(result.position).toEqual({ x: 1, y: 2, z: 3 });
        expect(result.scale).toEqual({ x: 4, y: 5, z: 6 });
    });

    test('resolveNodeQuatArgs handles Quat input', () => {
        const result = resolveNodeQuatArgs({ x: 1, y: 2, z: 3, w: 4 }, undefined, undefined, undefined, 9);
        expect(result).toEqual({ x: 1, y: 2, z: 3, w: 4 });
    });

    test('resolveNodeQuatArgs fills missing w from current value', () => {
        const result = resolveNodeQuatArgs(5, 6, 7, undefined, 8);
        expect(result).toEqual({ x: 5, y: 6, z: 7, w: 8 });
    });
});
