/**
 * blobboxes browser bundle — entry point.
 *
 * Self-contained JS module for extracting text bounding boxes from
 * rendered web pages and classifying them against domain filters.
 * Designed to run in a CDP isolated world or Qt WebEngine ApplicationWorld.
 *
 * API:
 *   blobboxes.init(opts)          — initialize, optionally start MutationObserver
 *   blobboxes.snapshot()          — one-shot extraction of all visible text bboxes
 *   blobboxes.loadFilters(arr)    — load domain filter bitmaps (Phase 3)
 *   blobboxes.classify(bboxes)    — classify bboxes against loaded filters (Phase 3)
 *   blobboxes.highlight(matches)  — CSS overlay for interactive mode
 *   blobboxes.dispose()           — cleanup observer and highlights
 */

import { extractBBoxes } from './bbox.js';
import { startObserver } from './observer.js';
import { fnv1a, classifyAll } from './classify.js';
import { highlight, clearHighlights } from './highlight.js';
import { RoaringBitmap32, roaringLibraryInitialize } from 'roaring-wasm';

const state = {
    config: null,
    disposeObserver: null,
    filters: [],
    roaringReady: false,
};

const blobboxes = {
    /**
     * Initialize the bundle. Call once after injection.
     *
     * @param {Object} [opts]
     * @param {Function} [opts.onMutation] - callback for batched bbox mutations
     * @param {number} [opts.flushInterval] - ms between observer flushes (default: 200)
     * @param {number} [opts.flushSize] - max batch size before forced flush (default: 50)
     * @param {number} [opts.maxTextLength] - skip text nodes longer than this (default: 200)
     */
    init(opts) {
        opts = opts || {};
        state.config = {
            maxTextLength: opts.maxTextLength || 200,
            flushInterval: opts.flushInterval || 200,
            flushSize: opts.flushSize || 50,
        };

        if (opts.onMutation) {
            state.disposeObserver = startObserver({
                onMutation: opts.onMutation,
                flushInterval: state.config.flushInterval,
                flushSize: state.config.flushSize,
                maxTextLength: state.config.maxTextLength,
            });
        }
    },

    /**
     * One-shot snapshot of all visible text bboxes.
     *
     * @returns {Array<Object>} bbox records
     */
    snapshot() {
        const maxLen = state.config ? state.config.maxTextLength : 200;
        return extractBBoxes({
            maxTextLength: maxLen,
            mutationType: "snapshot",
        });
    },

    /**
     * Load domain filters (portable roaring bitmaps from blobfilters).
     *
     * Requires roaring-wasm to be bundled. Each filter entry:
     *   { domain: string, bytes: Uint8Array }
     *
     * @param {Array<{domain: string, bytes: Uint8Array}>} filterData
     */
    async loadFilters(filterData) {
        if (!state.roaringReady) {
            await roaringLibraryInitialize();
            state.roaringReady = true;
        }

        // Dispose any previously loaded filters
        for (const f of state.filters) {
            f.bitmap.dispose();
        }

        state.filters = filterData.map(f => ({
            domain: f.domain,
            bitmap: RoaringBitmap32.deserialize("portable", f.bytes),
        }));
    },

    /**
     * Classify bboxes against loaded domain filters.
     *
     * @param {Array<Object>} bboxes - bbox records from snapshot() or observer
     * @returns {Array<{bbox: Object, matches: Array<{domain: string, score: number}>}>}
     */
    classify(bboxes) {
        if (state.filters.length === 0) {
            return [];
        }
        return classifyAll(bboxes, state.filters, RoaringBitmap32);
    },

    /**
     * Highlight classified bboxes with CSS overlays.
     *
     * @param {Array<{bbox: Object, matches: Array}>} classified
     * @param {Object} [opts] - { color: "rgba(...)" }
     */
    highlight(classified, opts) {
        highlight(classified, opts);
    },

    /**
     * Cleanup: stop observer, remove highlights, dispose filter bitmaps.
     */
    dispose() {
        if (state.disposeObserver) {
            state.disposeObserver();
            state.disposeObserver = null;
        }
        clearHighlights();
        for (const f of state.filters) {
            f.bitmap.dispose();
        }
        state.filters = [];
        state.config = null;
    },

    // Expose for direct use / testing
    fnv1a,
};

// Expose as global for injection via evaluate() / runJavaScript()
if (typeof globalThis !== 'undefined') {
    globalThis.blobboxes = blobboxes;
} else if (typeof window !== 'undefined') {
    window.blobboxes = blobboxes;
}

export default blobboxes;
