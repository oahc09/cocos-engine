import { Asset } from '../../../cocos/asset/assets/asset';
import {
    installScriptBridgeHelpers,
    registerScriptBridgeInstance,
    unregisterScriptBridgeInstance,
} from '../../../cocos/core/scripting/batch-executor';

class DummyAsset extends Asset {}

class DummyScriptComponent {
    public static __values__ = ['singleAsset', 'assetList', 'nonAsset'];

    public readonly start = jest.fn();
    public readonly update = jest.fn();
    public readonly lateUpdate = jest.fn();

    public singleAsset: Asset | null = null;
    public assetList: Array<Asset | null> = [];
    public nonAsset: unknown = null;
}

describe('script bridge batch executor', () => {
    afterEach(() => {
        unregisterScriptBridgeInstance(101);
        unregisterScriptBridgeInstance(202);
    });

    test('installs global batch and asset-ref helpers', () => {
        const target: Record<string, unknown> = {};

        installScriptBridgeHelpers(target as typeof globalThis);

        expect(typeof target.__scriptBridgeBatchCall).toBe('function');
        expect(typeof target.__scriptBridgeCollectAssetRefs).toBe('function');
    });

    test('invokes lifecycle methods on registered instances', () => {
        const target: Record<string, unknown> = {};
        installScriptBridgeHelpers(target as typeof globalThis);

        const instance = new DummyScriptComponent();
        registerScriptBridgeInstance(101, instance);

        (target.__scriptBridgeBatchCall as Function)([101], 'start');
        (target.__scriptBridgeBatchCall as Function)([101], 'update', 0.25);
        (target.__scriptBridgeBatchCall as Function)([101], 'lateUpdate', 0.5);

        expect(instance.start).toHaveBeenCalledTimes(1);
        expect(instance.update).toHaveBeenCalledWith(0.25);
        expect(instance.lateUpdate).toHaveBeenCalledWith(0.5);
    });

    test('ignores unregistered instances during batch execution', () => {
        const target: Record<string, unknown> = {};
        installScriptBridgeHelpers(target as typeof globalThis);

        const instance = new DummyScriptComponent();
        registerScriptBridgeInstance(101, instance);
        unregisterScriptBridgeInstance(101);

        (target.__scriptBridgeBatchCall as Function)([101], 'start');

        expect(instance.start).not.toHaveBeenCalled();
    });

    test('collects asset references from registered script properties', () => {
        const target: Record<string, unknown> = {};
        installScriptBridgeHelpers(target as typeof globalThis);

        const singleAsset = new DummyAsset();
        const arrayAssetA = new DummyAsset();
        const arrayAssetB = new DummyAsset();

        const instance = new DummyScriptComponent();
        instance.singleAsset = singleAsset;
        instance.assetList = [arrayAssetA, null, arrayAssetB];
        instance.nonAsset = 'ignore';

        registerScriptBridgeInstance(202, instance);

        const refs = (target.__scriptBridgeCollectAssetRefs as Function)(202) as Asset[];

        expect(refs).toEqual([singleAsset, arrayAssetA, arrayAssetB]);
    });
});
