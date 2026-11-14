import Foundation

struct PagerConfig: Codable, Equatable {
    var ownerName: String
    var greeting: String
    var tzMinutes: Int
    var canned: [String]
}

struct Voicemail: Codable, Identifiable, Equatable {
    let id: Int
    let ts: Int
    let number: String
    let name: String
    let duration: Int
    let transcript: String
    var heard: Bool

    var who: String { name.isEmpty ? (number.isEmpty ? "Unknown caller" : number) : name }
    var date: Date { Date(timeIntervalSince1970: TimeInterval(ts)) }
}

enum BackendError: LocalizedError {
    case notConfigured
    case http(Int, String)

    var errorDescription: String? {
        switch self {
        case .notConfigured: return "Enter your server address and app token in Settings first."
        case let .http(code, message): return "Server answered \(code). \(message)"
        }
    }
}

/// Client for the pager backend (backend/src/server.mjs).
struct Backend {
    let settings: Settings

    private func request(_ method: String, _ path: String, json: Data? = nil) throws -> URLRequest {
        guard settings.isConfigured, let base = settings.baseURL else { throw BackendError.notConfigured }
        var req = URLRequest(url: base.appendingPathComponent(path))
        req.httpMethod = method
        req.timeoutInterval = 8      // the bridge runs in a short background window
        req.setValue("Bearer \(settings.token)", forHTTPHeaderField: "Authorization")
        if let json {
            req.httpBody = json
            req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        }
        return req
    }

    private func send(_ req: URLRequest) async throws -> Data {
        let (data, response) = try await URLSession.shared.data(for: req)
        let code = (response as? HTTPURLResponse)?.statusCode ?? 0
        guard (200..<300).contains(code) else {
            let message = (try? JSONDecoder().decode([String: String].self, from: data))?["error"] ?? ""
            throw BackendError.http(code, message)
        }
        return data
    }

    /// Relay opaque frames from the pager; returns the frames to write back to it.
    func bridge(_ frames: [Data]) async throws -> [Data] {
        struct Body: Codable { let frames: [String] }
        let body = try JSONEncoder().encode(Body(frames: frames.map(\.hex)))
        let data = try await send(try request("POST", "v1/bridge", json: body))
        return try JSONDecoder().decode(Body.self, from: data).frames.compactMap(Data.init(hex:))
    }

    func config() async throws -> PagerConfig {
        try JSONDecoder().decode(PagerConfig.self, from: try await send(try request("GET", "v1/config")))
    }

    func save(_ config: PagerConfig) async throws -> PagerConfig {
        let data = try await send(try request("PUT", "v1/config", json: try JSONEncoder().encode(config)))
        return try JSONDecoder().decode(PagerConfig.self, from: data)
    }

    func voicemails() async throws -> [Voicemail] {
        struct Body: Codable { let voicemails: [Voicemail] }
        return try JSONDecoder().decode(Body.self, from: try await send(try request("GET", "v1/voicemails"))).voicemails
    }

    func audio(for id: Int) async throws -> Data {
        try await send(try request("GET", "v1/voicemails/\(id)/audio"))
    }

    func markHeard(_ id: Int) async throws { _ = try await send(try request("POST", "v1/voicemails/\(id)")) }
    func delete(_ id: Int) async throws { _ = try await send(try request("DELETE", "v1/voicemails/\(id)")) }

    func uploadContacts(_ contacts: [(number: String, name: String)]) async throws {
        struct Contact: Codable { let number: String; let name: String }
        struct Body: Codable { let contacts: [Contact] }
        let body = Body(contacts: contacts.map { Contact(number: $0.number, name: $0.name) })
        _ = try await send(try request("PUT", "v1/contacts", json: try JSONEncoder().encode(body)))
    }
}

extension Data {
    var hex: String { map { String(format: "%02x", $0) }.joined() }

    init?(hex: String) {
        guard hex.count % 2 == 0 else { return nil }
        var bytes = [UInt8]()
        var index = hex.startIndex
        while index < hex.endIndex {
            let next = hex.index(index, offsetBy: 2)
            guard let byte = UInt8(hex[index..<next], radix: 16) else { return nil }
            bytes.append(byte)
            index = next
        }
        self.init(bytes)
    }
}
