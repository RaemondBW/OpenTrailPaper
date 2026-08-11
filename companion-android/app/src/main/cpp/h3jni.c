// JNI bridge over h3shim.c — the same shim the iOS app calls through its
// bridging header, so both companions ask H3 exactly the same questions.
//
// Everything here works in plain scalars and primitive arrays: H3's own structs
// (CellBoundary and friends) are fixed C arrays that map badly onto the JVM, and
// the shim already flattens them.

#include <jni.h>
#include <stdlib.h>
#include <string.h>

#include "h3shim.h"

#define JNI_FN(name) Java_com_raemond_opentrailpaper_map_H3Native_##name

// Same cap as the Swift side (H3Tiles.coveringTiles): 4096 res-6 cells is a box
// roughly 350 km on a side, far past anything a rider selects in one drag.
#define COVERING_CAP 4096

JNIEXPORT jlongArray JNICALL
JNI_FN(coveringCells)(JNIEnv *env, jclass clazz,
                      jdouble south, jdouble west, jdouble north, jdouble east) {
    (void) clazz;
    uint64_t *buf = (uint64_t *) malloc(sizeof(uint64_t) * COVERING_CAP);
    if (!buf) return (*env)->NewLongArray(env, 0);

    int count = h3_covering_cells(south, west, north, east, buf, COVERING_CAP);
    if (count < 0) count = 0;

    jlongArray out = (*env)->NewLongArray(env, count);
    if (out && count > 0) {
        // uint64_t and jlong are both 64-bit; the sign reinterpretation is
        // harmless because Kotlin only ever hands these back to us.
        (*env)->SetLongArrayRegion(env, out, 0, count, (const jlong *) buf);
    }
    free(buf);
    return out;
}

JNIEXPORT jdoubleArray JNICALL
JNI_FN(cellBbox)(JNIEnv *env, jclass clazz, jlong cell) {
    (void) clazz;
    double s = 0, w = 0, n = 0, e = 0;
    h3_cell_bbox((uint64_t) cell, &s, &w, &n, &e);
    double vals[4] = {s, w, n, e};
    jdoubleArray out = (*env)->NewDoubleArray(env, 4);
    if (out) (*env)->SetDoubleArrayRegion(env, out, 0, 4, vals);
    return out;
}

JNIEXPORT jdoubleArray JNICALL
JNI_FN(cellBoundary)(JNIEnv *env, jclass clazz, jlong cell) {
    (void) clazz;
    double buf[24];            // up to 12 vertices x 2
    int n = h3_cell_boundary((uint64_t) cell, buf, 12);
    if (n < 0) n = 0;
    jdoubleArray out = (*env)->NewDoubleArray(env, n * 2);
    if (out && n > 0) (*env)->SetDoubleArrayRegion(env, out, 0, n * 2, buf);
    return out;
}

JNIEXPORT jstring JNICALL
JNI_FN(cellId)(JNIEnv *env, jclass clazz, jlong cell) {
    (void) clazz;
    char buf[17];
    memset(buf, 0, sizeof(buf));
    h3_cell_id((uint64_t) cell, buf, (int) sizeof(buf));
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jlong JNICALL
JNI_FN(fromId)(JNIEnv *env, jclass clazz, jstring id) {
    (void) clazz;
    if (!id) return 0;
    const char *s = (*env)->GetStringUTFChars(env, id, NULL);
    if (!s) return 0;
    uint64_t cell = h3_from_id(s);
    (*env)->ReleaseStringUTFChars(env, id, s);
    return (jlong) cell;
}

JNIEXPORT jlong JNICALL
JNI_FN(cellAt)(JNIEnv *env, jclass clazz, jdouble lat, jdouble lng) {
    (void) env;
    (void) clazz;
    return (jlong) h3_cell_at(lat, lng);
}
