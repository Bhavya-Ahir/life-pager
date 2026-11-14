# iPhone app

SwiftUI, iOS 17+. **Not compiled in this repository's CI or by its author's tooling** - it was
written without Xcode available, so expect to fix small compile errors on first build.

```bash
brew install xcodegen && cd ios && xcodegen && open LifePager.xcodeproj
```

Set your team and a bundle id of your own, then run it on your phone (CoreBluetooth does not
work in the simulator). A free Apple developer account is enough; with one, the app must be
re-installed every 7 days, so the $99 programme is worth it for daily use.

What the app does:

- **Bridge** (`PagerLink.swift`): stays connected to the pager in the background and relays
  opaque frames between it and your server. No UI needed after pairing.
- **Voicemail**: list, transcript, audio playback, delete.
- **Replies**: edit the canned messages, your name and the voicemail greeting.
- **Setup**: server address + token, forwarding codes, optional contact-name sync.

What the app deliberately does not do: show call alerts. iOS sends those straight to the
pager over ANCS, so they work even when the app has been force-quit.
