package com.raemond.opentrailpaper.ui

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.google.zxing.BarcodeFormat
import com.google.zxing.BinaryBitmap
import com.google.zxing.DecodeHintType
import com.google.zxing.PlanarYUVLuminanceSource
import com.google.zxing.common.HybridBinarizer
import com.google.zxing.qrcode.QRCodeReader
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Camera QR scanner for joining someone's mesh channel.
 *
 * CameraX with a ZXing analyser rather than ML Kit: one symbology and a single
 * callback do not need Play Services on the device, and the mesh exists precisely
 * for places where nothing else is reachable.
 *
 * The camera is bound to the composable's lifecycle, so it is live only while the
 * scanner is actually on screen — which is what the permission prompt promises.
 */
@Composable
fun MeshScannerView(modifier: Modifier = Modifier, onScan: (String) -> Unit) {
    val context = LocalContext.current
    var granted by remember { mutableStateOf(hasCamera(context)) }

    val ask = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted = it }

    LaunchedEffect(Unit) { if (!granted) ask.launch(Manifest.permission.CAMERA) }

    Box(modifier.fillMaxSize()) {
        if (granted) {
            CameraPreview(onScan)
        } else {
            Text(
                "Camera access is needed to scan a channel code. " +
                    "You can also paste the link instead.",
                style = barlow(15.sp),
                color = Palette.muted,
                textAlign = TextAlign.Center,
                modifier = Modifier.align(Alignment.Center).padding(32.dp),
            )
        }
    }
}

private fun hasCamera(context: Context): Boolean =
    ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) ==
        PackageManager.PERMISSION_GRANTED

@Composable
private fun CameraPreview(onScan: (String) -> Unit) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val view = remember {
        PreviewView(context).apply {
            // COMPATIBLE backs the preview with a TextureView. The default
            // PERFORMANCE mode uses a SurfaceView, which the system draws OVER
            // any Compose content composed before it — here that is the sheet's
            // own title bar and its Cancel button, i.e. the only way out.
            implementationMode = PreviewView.ImplementationMode.COMPATIBLE
        }
    }
    // One analysis thread, kept off the main one: ZXing on a 1280x720 frame is
    // milliseconds, but it is not free and it runs on every frame.
    val executor = remember { Executors.newSingleThreadExecutor() }
    // A code held in frame decodes on frame after frame; the parent only wants
    // the first, and dismissing takes a moment.
    val delivered = remember { AtomicBoolean(false) }

    DisposableEffect(Unit) {
        onDispose { executor.shutdown() }
    }

    AndroidView(modifier = Modifier.fillMaxSize(), factory = { view })

    LaunchedEffect(Unit) {
        val provider = ProcessCameraProvider.getInstance(context).await()
        val preview = Preview.Builder().build()
            .also { it.surfaceProvider = view.surfaceProvider }
        val analysis = ImageAnalysis.Builder()
            .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
            .build()
        analysis.setAnalyzer(executor) { image ->
            if (!delivered.get()) {
                qrText(image)?.let { text ->
                    if (delivered.compareAndSet(false, true)) {
                        view.post { onScan(text) }
                    }
                }
            }
            image.close()
        }
        runCatching {
            provider.unbindAll()
            provider.bindToLifecycle(
                lifecycleOwner, CameraSelector.DEFAULT_BACK_CAMERA, preview, analysis,
            )
        }
    }

    DisposableEffect(Unit) {
        onDispose {
            runCatching {
                ProcessCameraProvider.getInstance(context).get().unbindAll()
            }
        }
    }
}

/**
 * Decodes one camera frame, or null when it holds no QR.
 *
 * The Y plane of a YUV_420_888 frame IS the luminance ZXing wants, so there is no
 * colour conversion here — just the plane, its row stride and the crop the camera
 * reports.
 */
private fun qrText(image: ImageProxy): String? {
    val plane = image.planes.firstOrNull() ?: return null
    val buffer = plane.buffer
    val bytes = ByteArray(buffer.remaining())
    buffer.get(bytes)
    val rowStride = plane.rowStride
    // rowStride can exceed the width; PlanarYUVLuminanceSource takes the stride as
    // dataWidth, so passing the width instead skews the image into noise.
    val source = runCatching {
        PlanarYUVLuminanceSource(
            bytes, rowStride, image.height,
            0, 0, image.width, image.height, false,
        )
    }.getOrNull() ?: return null
    val hints = mapOf(
        DecodeHintType.POSSIBLE_FORMATS to listOf(BarcodeFormat.QR_CODE),
        DecodeHintType.TRY_HARDER to true,
    )
    return runCatching {
        QRCodeReader().decode(BinaryBitmap(HybridBinarizer(source)), hints).text
    }.getOrNull()
}

/** ListenableFuture -> coroutine, so the camera provider can be awaited inline. */
private suspend fun <T> com.google.common.util.concurrent.ListenableFuture<T>.await(): T =
    kotlinx.coroutines.suspendCancellableCoroutine { cont ->
        addListener(
            { runCatching { cont.resumeWith(Result.success(get())) } },
            Runnable::run,
        )
    }
