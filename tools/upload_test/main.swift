import Foundation

let service = IntervalsUploadService()
let fit = Data([14, 32, 0, 0, 0, 0, 0, 0, 46, 70, 73, 84, 0, 255])
let request = try service.request(fit, credential: "sample-key", boundary: "boundary")
precondition(request.url!.path == "/api/v1/athlete/0/activities")
precondition(request.value(forHTTPHeaderField: "Authorization") == "Basic " + Data("API_KEY:sample-key".utf8).base64EncodedString())
var expected = Data("--boundary\r\nContent-Disposition: form-data; name=\"file\"; filename=\"ride.fit\"\r\nContent-Type: application/octet-stream\r\n\r\n".utf8)
expected.append(fit)
expected.append(Data("\r\n--boundary--\r\n".utf8))
precondition(request.httpBody == expected)
precondition(request.url!.query!.contains("external_id=otp-" + rideUploadHash(fit)))
let retry = try service.request(fit, credential: "different-account")
precondition(request.url == retry.url)
let changed = try service.request(fit + Data([1]), credential: "sample-key")
precondition(changed.url != request.url)
let json = Data(#"{"id":"upload1","icu_athlete_id":"i123","activities":[{"id":"i456"}]}"#.utf8)
let created = try service.receipt(json, status: 201)
let duplicate = try service.receipt(json, status: 200)
precondition(created.activityID == "i456" && !created.alreadyUploaded)
precondition(duplicate.activityID == "i456" && duplicate.alreadyUploaded)
func mustFail(_ operation: () throws -> Void) {
    do { try operation(); fatalError("Accepted invalid upload") }
    catch { precondition(error is RideUploadError) }
}
for status in [202, 302, 400, 401, 403, 422, 429, 500] {
    mustFail { _ = try service.receipt(Data("secret account details".utf8), status: status) }
}
for body in ["{}", "[]", "<html>login</html>", #"{"activities":[]}"#, #"{"id":""}"#] {
    mustFail { _ = try service.receipt(Data(body.utf8), status: 201) }
}
mustFail { _ = try service.request(fit, credential: " ") }
mustFail { _ = try service.request(Data([1, 2]), credential: "key") }
print("Upload contract tests passed")
