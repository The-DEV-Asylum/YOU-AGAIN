// Test polyfills for jsdom (Vitest's DOM). Ionic components like ion-menu and
// ion-split-pane call window.matchMedia, which jsdom does not have. So we
// fake one. It always says "no match". It is very confident about it.
if (!window.matchMedia) {
  window.matchMedia = (query: string): MediaQueryList =>
    ({
      matches: false,
      media: query,
      onchange: null,
      addListener: () => undefined,
      removeListener: () => undefined,
      addEventListener: () => undefined,
      removeEventListener: () => undefined,
      dispatchEvent: () => false,
    }) as MediaQueryList;
}
