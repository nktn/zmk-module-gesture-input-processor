// jest-dom adds custom jest matchers for asserting on DOM nodes.
import "@testing-library/jest-dom";

// jsdom's global scope does not provide TextEncoder/TextDecoder, but
// protobuf encoding (used by generated proto code) requires them.
import { TextEncoder, TextDecoder } from "node:util";

if (typeof globalThis.TextEncoder === "undefined") {
  // @ts-expect-error Node's TextEncoder is close enough to lib.dom's for tests
  globalThis.TextEncoder = TextEncoder;
}
if (typeof globalThis.TextDecoder === "undefined") {
  // @ts-expect-error Node's TextDecoder is close enough to lib.dom's for tests
  globalThis.TextDecoder = TextDecoder;
}
