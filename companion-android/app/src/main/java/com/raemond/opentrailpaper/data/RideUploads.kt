package com.raemond.opentrailpaper.data

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.AtomicFile
import androidx.compose.runtime.mutableStateMapOf
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** Secrets stay outside backups, encrypted with a non-exportable Android Keystore key. */
class UploadCredentials(context: Context) {
    private val directory = File(context.noBackupFilesDir, "upload-services")
    private fun file(service: String) = AtomicFile(File(directory, "$service.key"))
    private fun key(): SecretKey {
        val alias = "opentrailpaper-upload-services"
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(alias, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").apply {
            init(KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build())
        }.generateKey()
    }
    @Synchronized fun read(service: String): String {
        val file = file(service)
        if (!file.baseFile.exists()) return ""
        try {
            val bytes = file.readFully()
            require(bytes.size >= 28)
            return Cipher.getInstance("AES/GCM/NoPadding").run {
                init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, bytes.copyOfRange(0, 12)))
                String(doFinal(bytes.copyOfRange(12, bytes.size)), Charsets.UTF_8)
            }
        } catch (e: Exception) {
            throw RideUploadException("Could not read the saved API key. Save it again in Upload services.")
        }
    }
    @Synchronized fun save(service: String, credential: String) {
        val file = file(service)
        if (credential.isEmpty()) {
            file.delete()
            if (file.baseFile.exists()) throw RideUploadException("Could not remove the saved API key.")
            return
        }
        try {
            check(directory.isDirectory || directory.mkdirs())
            val cipher = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, key()) }
            val bytes = cipher.iv + cipher.doFinal(credential.toByteArray(Charsets.UTF_8))
            val stream = file.startWrite()
            try { stream.write(bytes); file.finishWrite(stream) }
            catch (e: Exception) { file.failWrite(stream); throw e }
        } catch (e: Exception) {
            throw RideUploadException("Could not save the API key securely. Try again.")
        }
    }
}
data class RideUploadStatus(val busy: Boolean = false, val message: String = "")

// Jobs survive sheet dismissal and rotation. Receipts are scoped to service,
// account-key fingerprint and FIT contents. No automatic POST retries.
class RideUploads private constructor(context: Context) {
    val services: List<RideUploadService> = listOf(IntervalsUploadService())
    val credentials = UploadCredentials(context)
    private val receipts = context.getSharedPreferences("ride-upload-receipts", Context.MODE_PRIVATE)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    val status = mutableStateMapOf<String, RideUploadStatus>()
    fun actionID(service: String, file: File) = "$service:${file.absolutePath}"
    fun upload(service: RideUploadService, file: File) {
        val action = actionID(service.id, file)
        if (status[action]?.busy == true) return
        status[action] = RideUploadStatus(busy = true)
        scope.launch {
            status[action] = try {
                withContext(Dispatchers.IO) {
                    val credential = credentials.read(service.id)
                    val data = file.readBytes()
                    val receiptKey = "${service.id}.${rideUploadHash(credential.toByteArray())}.${rideUploadHash(data)}"
                    val already = receipts.contains(receiptKey)
                    val receipt = if (already) null else service.upload(data, credential)
                    if (receipt != null) receipts.edit().putString(receiptKey, receipt.activityID).commit()
                    RideUploadStatus(message = if (already || receipt?.alreadyUploaded == true)
                        "Already uploaded to ${service.name}." else "Uploaded to ${service.name}.")
                }
            } catch (e: Exception) {
                RideUploadStatus(message = (e as? RideUploadException)?.message
                    ?: "Could not read the saved ride. Download it again and retry.")
            }
        }
    }
    companion object {
        @Volatile private var instance: RideUploads? = null
        fun get(context: Context): RideUploads = instance ?: synchronized(this) {
            instance ?: RideUploads(context.applicationContext).also { instance = it }
        }
    }
}
