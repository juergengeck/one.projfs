import { IFileSystem } from '@refinio/one.models/lib/fileSystems/IFileSystem.js';

export interface IFSProjFSProviderOptions {
    instancePath: string;
    virtualRoot: string;
    fileSystem: IFileSystem;
    debug?: boolean;
}

export class IFSProjFSProvider {
    constructor(options: IFSProjFSProviderOptions | string);
    mount(): Promise<void>;
    unmount(): Promise<void>;
    getCacheStats?(): any;
}
