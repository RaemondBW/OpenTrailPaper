package com.raemond.opentrailpaper.data

// Display-unit conversions. Ride data is stored/decoded in metric; these convert
// only at display time based on the user's preference, which lives in Prefs and
// is mirrored to the device with every settings push.
object Units {
    fun distance(km: Double, miles: Boolean): Double = if (miles) km * 0.621371 else km
    fun speed(kmh: Double, miles: Boolean): Double = if (miles) kmh * 0.621371 else kmh
    fun elevation(m: Double, miles: Boolean): Double = if (miles) m * 3.28084 else m

    fun distLabel(miles: Boolean): String = if (miles) "mi" else "km"
    fun speedLabel(miles: Boolean): String = if (miles) "mph" else "km/h"
    fun elevLabel(miles: Boolean): String = if (miles) "ft" else "m"
}
