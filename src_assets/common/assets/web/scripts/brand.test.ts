import assert from "node:assert/strict";
import test from "node:test";
// @ts-expect-error: plain ES module without types
import { brand } from "./brand.mjs";

test("the product name becomes the brand", () => {
  for (const [from, to] of [
    ["Open Vibepollo", "Open Scirocco"],
    ["Restart Vibepollo.", "Restart Scirocco."],
    ["Vibepollo's latest", "Scirocco's latest"],
    ['"Vibepollo will', '"Scirocco will'],
    ["a Vibepollo-ban", "a Scirocco-ban"],
  ]) {
    assert.equal(brand(from, "Scirocco"), to);
  }
});

test("drivers, the Playnite plugin, files and URLs keep their names", () => {
  for (const kept of [
    "Vibepollo Display Driver",
    "the Vibepollo virtual display",
    "Vibepollo Virtual Gamepad",
    "Vibepollo Playnite plugin",
    "Vibepollo.msi",
    "VibepolloSetup.exe",
    "Vibepollo-x86_64.AppImage",
    "https://github.com/Nonary/Vibepollo/releases",
    "vibepollo",
  ]) {
    assert.equal(brand(kept, "Scirocco"), kept);
  }
});
