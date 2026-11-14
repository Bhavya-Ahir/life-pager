import AVFoundation
import Contacts
import SwiftUI

struct RootView: View {
    @EnvironmentObject private var settings: Settings

    var body: some View {
        TabView {
            StatusView().tabItem { Label("Pager", systemImage: "dot.radiowaves.left.and.right") }
            VoicemailView().tabItem { Label("Voicemail", systemImage: "recordingtape") }
            RepliesView().tabItem { Label("Replies", systemImage: "text.bubble") }
            SetupView().tabItem { Label("Setup", systemImage: "gearshape") }
        }
    }
}

// MARK: - Status

struct StatusView: View {
    @EnvironmentObject private var link: PagerLink
    @EnvironmentObject private var settings: Settings

    var body: some View {
        NavigationStack {
            List {
                Section("Bluetooth") {
                    LabeledContent("Pager", value: link.status.rawValue)
                    if let last = link.lastRelay {
                        LabeledContent("Last relay", value: last.formatted(date: .omitted, time: .standard))
                    }
                    if let error = link.lastError {
                        Text(error).font(.footnote).foregroundStyle(.red)
                    }
                }
                if !settings.isConfigured {
                    Section {
                        Text("Call alerts already work over Bluetooth. Add your server in Setup to enable text replies and voicemail.")
                            .font(.footnote)
                    }
                }
                Section("How it works") {
                    Text("Calls reach the pager directly from iOS. This app only relays replies and voicemail between the pager and your server, in the background. You can leave it closed.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            }
            .navigationTitle("Life Pager")
        }
    }
}

// MARK: - Voicemail

@MainActor
final class VoicemailModel: ObservableObject {
    @Published var items: [Voicemail] = []
    @Published var error: String?
    @Published var playing: Int?
    private var player: AVAudioPlayer?

    func load(_ backend: Backend) async {
        do { items = try await backend.voicemails(); error = nil } catch { self.error = error.localizedDescription }
    }

    func toggle(_ vm: Voicemail, _ backend: Backend) async {
        if playing == vm.id {
            player?.stop()
            playing = nil
            return
        }
        do {
            let data = try await backend.audio(for: vm.id)
            try AVAudioSession.sharedInstance().setCategory(.playback)
            player = try AVAudioPlayer(data: data)
            player?.play()
            playing = vm.id
            if !vm.heard {
                try await backend.markHeard(vm.id)
                await load(backend)
            }
        } catch { self.error = error.localizedDescription }
    }

    func delete(_ vm: Voicemail, _ backend: Backend) async {
        do { try await backend.delete(vm.id); await load(backend) } catch { self.error = error.localizedDescription }
    }
}

struct VoicemailView: View {
    @EnvironmentObject private var settings: Settings
    @StateObject private var model = VoicemailModel()

    var body: some View {
        let backend = Backend(settings: settings)
        NavigationStack {
            List {
                if let error = model.error { Text(error).font(.footnote).foregroundStyle(.red) }
                ForEach(model.items) { vm in
                    VStack(alignment: .leading, spacing: 6) {
                        HStack {
                            Text(vm.who).font(.headline).fontWeight(vm.heard ? .regular : .bold)
                            Spacer()
                            Text(vm.date.formatted(date: .abbreviated, time: .shortened))
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        Text(vm.transcript.isEmpty ? "No transcript" : vm.transcript)
                            .font(.subheadline).foregroundStyle(vm.transcript.isEmpty ? .secondary : .primary)
                        Button {
                            Task { await model.toggle(vm, backend) }
                        } label: {
                            Label(model.playing == vm.id ? "Stop" : "Play \(vm.duration)s",
                                  systemImage: model.playing == vm.id ? "stop.fill" : "play.fill")
                        }
                        .buttonStyle(.bordered)
                    }
                    .swipeActions {
                        Button("Delete", role: .destructive) { Task { await model.delete(vm, backend) } }
                    }
                }
                if model.items.isEmpty && model.error == nil {
                    Text("No voicemail.").foregroundStyle(.secondary)
                }
            }
            .navigationTitle("Voicemail")
            .refreshable { await model.load(backend) }
            .task { await model.load(backend) }
        }
    }
}

// MARK: - Canned replies

struct RepliesView: View {
    @EnvironmentObject private var settings: Settings
    @State private var config: PagerConfig?
    @State private var message: String?

    var body: some View {
        let backend = Backend(settings: settings)
        NavigationStack {
            Form {
                if let message { Text(message).font(.footnote).foregroundStyle(.secondary) }
                if config != nil {
                    Section {
                        ForEach(config!.canned.indices, id: \.self) { index in
                            TextField("Reply", text: Binding(
                                get: { config!.canned[index] },
                                set: { config!.canned[index] = String($0.prefix(160)) }), axis: .vertical)
                        }
                        .onDelete { config!.canned.remove(atOffsets: $0) }
                        if config!.canned.count < 8 {
                            Button("Add reply") { config!.canned.append("") }
                        }
                    } header: {
                        Text("Replies the pager can send")
                    } footer: {
                        Text("Sent as a text from your pager number, starting with \"\(config!.ownerName) (auto-reply from my pager):\". The pager shows the first 21 characters, so start each one differently.")
                    }
                    Section("Shown to callers") {
                        TextField("Your name", text: Binding(get: { config!.ownerName }, set: { config!.ownerName = $0 }))
                        TextField("Voicemail greeting", text: Binding(get: { config!.greeting }, set: { config!.greeting = $0 }), axis: .vertical)
                    }
                    Button("Save to pager") {
                        Task {
                            do {
                                var next = config!
                                next.canned = next.canned.map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
                                next.tzMinutes = TimeZone.current.secondsFromGMT() / 60
                                config = try await backend.save(next)
                                message = "Saved. The pager picks it up the next time it connects."
                            } catch { message = error.localizedDescription }
                        }
                    }
                }
            }
            .navigationTitle("Replies")
            .task {
                do { config = try await backend.config(); message = nil } catch { message = error.localizedDescription }
            }
        }
    }
}

// MARK: - Setup

struct SetupView: View {
    @EnvironmentObject private var settings: Settings
    @State private var forwardNumber = ""
    @State private var contactsMessage: String?

    /// Uploads name + number for every contact. Numbers without a country code are assumed to be
    /// in the phone's own region when that is +1; other regions should store contacts with "+".
    private func syncContacts() async {
        let store = CNContactStore()
        do {
            guard try await store.requestAccess(for: .contacts) else {
                contactsMessage = "Contacts access was not granted."
                return
            }
            var out: [(number: String, name: String)] = []
            let keys = [CNContactGivenNameKey, CNContactFamilyNameKey, CNContactPhoneNumbersKey] as [CNKeyDescriptor]
            try store.enumerateContacts(with: CNContactFetchRequest(keysToFetch: keys)) { contact, _ in
                let name = [contact.givenName, contact.familyName].filter { !$0.isEmpty }.joined(separator: " ")
                guard !name.isEmpty else { return }
                for phone in contact.phoneNumbers {
                    let raw = phone.value.stringValue
                    let digits = raw.filter(\.isNumber)
                    if raw.trimmingCharacters(in: .whitespaces).hasPrefix("+") {
                        out.append(("+" + digits, name))
                    } else if digits.count == 10, Locale.current.region?.identifier == "US" {
                        out.append(("+1" + digits, name))
                    }
                }
            }
            try await Backend(settings: settings).uploadContacts(out)
            contactsMessage = "Synced \(out.count) numbers."
        } catch { contactsMessage = error.localizedDescription }
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("https://pager.example.com", text: $settings.serverURL)
                        .textInputAutocapitalization(.never).autocorrectionDisabled().keyboardType(.URL)
                    SecureField("App token", text: $settings.token)
                } header: {
                    Text("Your server")
                } footer: {
                    Text(settings.isConfigured ? "Ready." : "Needs an https address and the APP_TOKEN from your server's .env file.")
                }
                Section {
                    TextField("+1 555 000 0000", text: $forwardNumber).keyboardType(.phonePad)
                    let digits = forwardNumber.filter { $0.isNumber || $0 == "+" }
                    if digits.count >= 8 {
                        // iOS will not dial tel: links that contain * or #, so hand over the code instead.
                        Button("Copy \"turn on\" code") { UIPasteboard.general.string = "**004*\(digits)#" }
                        Button("Copy \"turn off\" code") { UIPasteboard.general.string = "##004#" }
                    }
                } header: {
                    Text("Send missed calls to the pager number")
                } footer: {
                    Text("Paste the code into the Phone keypad and press call. It is the standard GSM code that forwards calls you decline, miss, or cannot receive to your Twilio number. This replaces your carrier voicemail. Verizon and some prepaid plans use different codes - see docs/build-guide.md.")
                }
                Section {
                    Button("Sync contact names") { Task { await syncContacts() } }
                    if let contactsMessage { Text(contactsMessage).font(.footnote).foregroundStyle(.secondary) }
                } footer: {
                    Text("Optional. Lets the pager show names instead of numbers when it is away from the phone. Names and numbers are stored on your own server only.")
                }
                Section("Pair the pager") {
                    Text("1. Hold the pager near the phone and open this app; accept the pairing request.\n2. Accept \"Allow Life Pager to receive your notifications\". That is what lets it show incoming calls.\n3. In Settings > Bluetooth > Life Pager, keep Share System Notifications on.")
                        .font(.footnote)
                }
            }
            .navigationTitle("Setup")
        }
    }
}
