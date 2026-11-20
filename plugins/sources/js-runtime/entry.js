import { Buffer } from "buffer";
import { URL, URLSearchParams } from "whatwg-url";
import crypto from "@shiqianjiang/ceru-plugin-sdk/compat/crypto";
import compression from "@shiqianjiang/ceru-plugin-sdk/compat/zlib";
import { createLegacyHttpBridge } from "@shiqianjiang/ceru-plugin-sdk/legacy-http";
import { createHttpClient } from "@shiqianjiang/ceru-plugin-sdk/http";

export { Buffer, URL, URLSearchParams, crypto, compression, createLegacyHttpBridge, createHttpClient };
