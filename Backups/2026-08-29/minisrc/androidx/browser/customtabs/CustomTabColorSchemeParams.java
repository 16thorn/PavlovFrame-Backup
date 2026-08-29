package androidx.browser.customtabs;
// Minimal stand-in: EOS SDK only calls Builder(){}, setToolbarColor(int), build().
// The value object is passed to CustomTabsIntent.Builder.setDefaultColorSchemeParams,
// which is patched to a no-op, so nothing here is ever read.
public final class CustomTabColorSchemeParams {
    public static final class Builder {
        public Builder() {}
        public Builder setToolbarColor(int color) { return this; }
        public CustomTabColorSchemeParams build() { return new CustomTabColorSchemeParams(); }
    }
}
