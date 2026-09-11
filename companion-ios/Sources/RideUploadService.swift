import Foundation
import CryptoKit

// Each provider owns authentication and its wire protocol. Ride screens only
// deal in original FIT bytes and receipts; OAuth providers can implement this
// same interface without changing the recorder or BLE download protocol.
protocol RideUploadService {
    var id: String { get }
    var name: String { get }
    var credentialHelpURL: URL { get }
    func upload(_ data: Data, credential: String) async throws -> RideUploadReceipt
}

struct RideUploadReceipt: Codable {
    let activityID: String
    let alreadyUploaded: Bool
}

struct RideUploadError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}

func rideUploadHash(_ data: Data) -> String {
    SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
}

struct IntervalsUploadService: RideUploadService {
    let id = "intervals"
    let name = "Intervals.icu"
    let credentialHelpURL = URL(string: "https://intervals.icu/settings")!
    var session: URLSession = .shared

    func request(_ data: Data, credential: String, boundary: String = UUID().uuidString) throws -> URLRequest {
        guard !credential.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            throw RideUploadError(message: "Add your Intervals.icu API key in Upload services first.")
        }
        guard data.count >= 12, data.subdata(in: 8..<12) == Data(".FIT".utf8) else {
            throw RideUploadError(message: "This file is not a FIT ride. Download it again from the device.")
        }
        var url = URLComponents(string: "https://intervals.icu/api/v1/athlete/0/activities")!
        url.queryItems = [URLQueryItem(name: "device_name", value: "OpenTrailPaper"),
                          URLQueryItem(name: "external_id", value: "otp-" + rideUploadHash(data))]
        var request = URLRequest(url: url.url!, timeoutInterval: 90)
        request.httpMethod = "POST"
        request.setValue("Basic " + Data("API_KEY:\(credential)".utf8).base64EncodedString(),
                         forHTTPHeaderField: "Authorization")
        request.setValue("application/json", forHTTPHeaderField: "Accept")
        request.setValue("multipart/form-data; boundary=\(boundary)", forHTTPHeaderField: "Content-Type")
        // Fixed filename avoids header injection and preserves identical bytes
        // on retries. Intervals also deduplicates by a hash of those bytes.
        var body = Data("--\(boundary)\r\nContent-Disposition: form-data; name=\"file\"; filename=\"ride.fit\"\r\nContent-Type: application/octet-stream\r\n\r\n".utf8)
        body.append(data)
        body.append(Data("\r\n--\(boundary)--\r\n".utf8))
        request.httpBody = body
        return request
    }

    func receipt(_ data: Data, status: Int) throws -> RideUploadReceipt {
        switch status {
        case 200, 201: break
        case 401, 403:
            throw RideUploadError(message: "Intervals.icu rejected the API key. Check it in Upload services.")
        case 429:
            throw RideUploadError(message: "Intervals.icu is limiting uploads. Wait a few minutes and try again.")
        case 400, 422:
            throw RideUploadError(message: "Intervals.icu could not read this ride. Try downloading the FIT file again.")
        default:
            throw RideUploadError(message: "Intervals.icu could not confirm the upload (HTTP \(status)). You can retry safely.")
        }
        struct Response: Decodable {
            struct Activity: Decodable { let id: String }
            let id: String?
            let activities: [Activity]?
        }
        guard let response = try? JSONDecoder().decode(Response.self, from: data),
              let id = response.activities?.first?.id ?? response.id, !id.isEmpty else {
            throw RideUploadError(message: "Intervals.icu did not confirm an activity. You can retry safely.")
        }
        return RideUploadReceipt(activityID: id, alreadyUploaded: status == 200)
    }

    func upload(_ data: Data, credential: String) async throws -> RideUploadReceipt {
        let request = try request(data, credential: credential)
        do {
            let (body, response) = try await session.data(for: request)
            guard let response = response as? HTTPURLResponse else {
                throw RideUploadError(message: "No response from Intervals.icu. You can retry safely.")
            }
            return try receipt(body, status: response.statusCode)
        } catch let error as RideUploadError { throw error }
        catch {
            // Do not expose request/credential details or assume a timed-out
            // request failed to reach the server. No automatic POST retries.
            throw RideUploadError(message: "Upload was not confirmed. Check your connection and retry; duplicate rides are detected automatically.")
        }
    }
}
