/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

import type { RenderOptions } from "ddnet-loader";

/** Where the program's script is, for a page that wants to fetch it early. */
export declare const programUrl: string;

/**
 * Writes a video of a demo. Everything `ddnet-loader`'s `render` takes may be
 * said here as well; which program to run and where it is need not be.
 */
export declare function renderDemo(options: Omit<RenderOptions, "module" | "moduleName" | "scriptUrl"> & {
	module?: RenderOptions["module"];
	scriptUrl?: string;
}): Promise<File | Blob | null>;

declare const DDNetDemoRenderer: {
	renderDemo: typeof renderDemo;
	programUrl: typeof programUrl;
	loader: unknown;
};
export default DDNetDemoRenderer;
