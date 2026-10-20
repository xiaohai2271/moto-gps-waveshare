package dev.glimpse.motogps

/**
 * Thin JNI facade over the shared C++ core (shared/ble_protocol,
 * shared/coordinates). All protocol bytes come from the upstream implementation
 * so the Android app cannot silently drift from the v1 wire format.
 */
object NativeBridge {
    init {
        System.loadLibrary("moto_core")
    }

    external fun crc16CcittFalse(bytes: ByteArray): Int

    /**
     * Fragments one encoded payload into complete GATT frames.
     * [applicationFlags] may only contain ACK_REQUESTED(4) and URGENT(8);
     * START/END are assigned by the shared implementation.
     */
    external fun fragmentMessage(
        messageType: Int,
        sequence: Int,
        applicationFlags: Int,
        maxFrameSize: Int,
        payload: ByteArray,
    ): Array<ByteArray>

    external fun decodeFrame(frameBytes: ByteArray): ByteArray

    /** Returns `key=hex` lines identical in format to the upstream golden fixture. */
    external fun goldenVectors(): String

    /** @return [longitude, latitude, ok(1.0/0.0)] */
    external fun wgs84ToGcj02(lon: Double, lat: Double): DoubleArray

    // --- BLE session codec (shared/ble_protocol) ---

    external fun encodeConnectionStatus(
        role: Int,
        state: Int,
        minimumVersion: Int,
        maximumVersion: Int,
        capabilities: Long,
        sessionId: Long,
        maxFrameSize: Int,
        heartbeatIntervalMs: Int,
    ): ByteArray

    external fun encodeHeartbeat(
        sessionId: Long,
        monotonicMs: Long,
        statusFlags: Int,
    ): ByteArray

    external fun encodeAck(
        acknowledgedSequence: Int,
        status: Int,
        commandId: Int,
    ): ByteArray

    /** @return [revision, role, state, minVersion, maxVersion, capabilities, sessionId, maxFrameSize, heartbeatIntervalMs] */
    external fun decodeConnectionStatus(payload: ByteArray): LongArray

    /** @return [revision, acknowledgedSequence, status, commandId] */
    external fun decodeAck(payload: ByteArray): IntArray

    /** @return [revision, kind, commandId, page, x, y, eventTimeMs] */
    external fun decodeDeviceCommand(payload: ByteArray): LongArray

    external fun createReassembler(timeoutMs: Long): Long

    /** @return "state=..,error=..,dropped=..,type=..,version=..,flags=..,seq=..,payload=hex" */
    external fun reassemblerPush(handle: Long, frame: ByteArray, nowMs: Long): String

    external fun reassemblerReset(handle: Long)

    external fun destroyReassembler(handle: Long)

    // --- Shared navigation core (shared/nav_app) ---

    external fun navCreate(): Long
    external fun navDestroy(handle: Long)
    external fun navReset(handle: Long): String
    external fun navBeginNavigation(handle: Long, lon: Double, lat: Double): String
    external fun navCancelNavigation(handle: Long): String
    external fun navSetNetwork(handle: Long, network: Int): String
    external fun navSelectPage(handle: Long, page: Int): String
    external fun navPushFix(
        handle: Long,
        lon: Double,
        lat: Double,
        accuracyM: Double,
        speedMps: Double,
        headingDeg: Double,
        timestampMs: Long,
    ): String
    external fun navTick(handle: Long, nowMs: Long): String
    external fun navSnapshot(handle: Long): String

    external fun navAcceptRoute(
        handle: Long,
        requestId: Int,
        routeId: String,
        totalDistanceM: Double,
        totalDurationS: Double,
        speedLimitKph: Int,
        generatedAtMs: Long,
        receivedAtMs: Long,
        polylineLonLat: DoubleArray,
        maneuverNumeric: IntArray,
        maneuverRoads: Array<String>,
        maneuverInstructions: Array<String>,
        traffic: DoubleArray,
    ): String

    external fun navRejectRoute(
        handle: Long,
        requestId: Int,
        retryable: Boolean,
        receivedAtMs: Long,
    ): String

    external fun navAcceptTraffic(
        handle: Long,
        requestId: Int,
        routeId: String,
        remainingDurationS: Int,
        observedAtMs: Long,
        segments: DoubleArray,
    ): String

    // --- BLE display encoders ---

    external fun bleRouteToken(routeId: String): Long

    /**
     * numeric[20] = state, network, displayPage, maneuver, traffic, flags,
     * routeToken, generation, maneuverId, distanceToManeuverM,
     * remainingDistanceM, remainingDurationS, routeProgressM, totalDistanceM,
     * speedDeciKph, speedLimitKph, headingCdeg, accuracyDm, crossTrackDm,
     * roundaboutExit
     */
    external fun encodeNavigationSnapshot(
        numeric: LongArray,
        roadName: String,
        instruction: String,
    ): ByteArray

    /**
     * meta[9] = coordinateSystem, routeToken, generation, chunkIndex,
     * chunkCount, firstPointIndex, totalPointCount, originLatE6, originLonE6
     */
    external fun encodeRouteGeometry(meta: LongArray, pointsLonLat: DoubleArray): ByteArray

    /**
     * meta[4] = sceneRevision, originLatE6, originLonE6, radiusM.
     * roadRanges / buildingRanges are (startIndex, pointCount) pairs into the
     * corresponding interleaved lon/lat arrays.
     */
    external fun encodeMapScene(
        meta: LongArray,
        roadClasses: IntArray,
        roadRanges: IntArray,
        roadPointsLonLat: DoubleArray,
        buildingClasses: IntArray,
        buildingRanges: IntArray,
        buildingPointsLonLat: DoubleArray,
    ): ByteArray

    external fun encodeMediaState(
        flags: Int,
        trackToken: Long,
        positionS: Int,
        durationS: Int,
        sourceName: String,
        trackTitle: String,
        artistName: String,
    ): ByteArray

    /**
     * Encodes one UiLayout (protocol §UiLayout). The shared C++ codec owns the
     * byte format; Kotlin only maps the model onto the int arrays.
     *
     * fixed[15] = layoutRevision, flags, themeId, mapHeightPx, mapOpacity,
     *   routeRgb(0xRRGGBB), routeWidthPx, routeOpacity, streetRgb(0xRRGGBB),
     *   streetWidthPx, streetOpacity, buildingOpacity, layerMask, zoomPercent,
     *   componentCount
     * components[componentCount * 8] = per component
     *   [id, cflags, size, opacity, x, y, anchor, spanDeg]
     * Wire constraints (violations raise IllegalStateException):
     *   - componentCount == components.size / 8, ids unique in 1..8
     *   - size in 0..2, opacity in 0..100
     *   - arcs (id 7/8): x == y == 0, anchor in 0..3, spanDeg in {90,120,180}
     *   - non-arcs: anchor == spanDeg == 0, x/y within the 466 canvas
     */
    external fun encodeUiLayout(fixed: IntArray, components: IntArray): ByteArray

    /** @return [revision, layoutRevision, source, supportedMask, flags, reserved] */
    external fun decodeUiLayoutState(payload: ByteArray): IntArray

    /**
     * WiFi provisioning (docs §5.5). Encoding goes through the shared C++
     * codec like every other message; only the int-array mapping is
     * duplicated here.
     *
     * encodeWifiConfig(action, flags, ssid, password) — action 0..3,
     * flags bit0 hidden, bit1 auto_reconnect; ssid 1..32, password 0..64
     * bytes; disconnect(2)/forget(3) must send empty credentials.
     */
    external fun encodeWifiConfig(
        action: Int,
        flags: Int,
        ssid: String,
        password: String,
    ): ByteArray

    /** decodeWifiStatus → [state, rssi, error, ipv4Le, ssid] (ssid last). */
    external fun decodeWifiStatus(payload: ByteArray): Array<Any>

    /** encodeWifiScanRequest(requestId) — requestId must be > 0. */
    external fun encodeWifiScanRequest(requestId: Int): ByteArray

    /**
     * decodeWifiScanResult → [requestId, apCount, then per AP:
     * rssi, auth, ssidBytes(≤32)]; the Kotlin caller rebuilds the typed list.
     */
    external fun decodeWifiScanResult(payload: ByteArray): Array<Any>
}
