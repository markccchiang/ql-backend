# Logo

The mark is a **Q**: the bowl is QuantLib, the tail is a short curve as a price
curve is, and it ends in an orange handle — the quote a user drags, and the
reason the service is interactive at all.

| File | Use it on |
| --- | --- |
| `logo.svg` | light grounds — README headers, slides |
| `logo-dark.svg` | dark grounds |
| `mark.svg` | favicons, avatars, anywhere square; reads on light and dark |
| `mark-dark.svg` | dark grounds where the brighter teal is wanted — the docs sidebar |

On GitHub, let the reader's theme pick:

```html
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/logo/logo-dark.svg">
  <img src="assets/logo/logo.svg" alt="Interactive QuantLib Service" width="560">
</picture>
```

In a web app, `mark.svg` is a favicon as it stands:
`<link rel="icon" type="image/svg+xml" href="/mark.svg">`.

**Colours** are Mantine's, because the frontend's theme already is: teal 7
`#0ca678` on light and teal 5 `#20c997` on dark, orange 6 `#fd7e14` for the
handle, ink `#1a1b1e` and paper `#f1f3f5` for the name. The mark holds at
16 px; below that, use nothing rather than a smaller one.

**The wordmark is outlines**, not text, set in
[Inter](https://rsms.me/inter/) (SIL Open Font License 1.1), so no file here
depends on a font being installed. Edit `generate.py` and rerun it rather than
editing the SVGs by hand — they are its output.
