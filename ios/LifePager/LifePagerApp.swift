import SwiftUI

@main
struct LifePagerApp: App {
    // Created at launch, including background relaunches by CoreBluetooth state restoration.
    @StateObject private var settings: Settings
    @StateObject private var link: PagerLink

    init() {
        let settings = Settings()
        _settings = StateObject(wrappedValue: settings)
        _link = StateObject(wrappedValue: PagerLink(settings: settings))
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(settings)
                .environmentObject(link)
        }
    }
}
