/**
 * MutationObserver with batched flush.
 *
 * Watches for DOM mutations (added/changed text nodes) and batches
 * bbox extractions, flushing on an interval or when the batch is full.
 */

import { extractBBoxes } from './bbox.js';

/**
 * Start observing DOM mutations, extracting bboxes from affected subtrees.
 *
 * @param {Object} opts
 * @param {Function} opts.onMutation - callback receiving batched bbox arrays
 * @param {number} opts.flushInterval - ms between flushes (default: 200)
 * @param {number} opts.flushSize - max batch size before forced flush (default: 50)
 * @param {number} opts.maxTextLength - passed to extractBBoxes
 * @returns {Function} dispose function to stop observing
 */
export function startObserver(opts) {
    const onMutation = opts.onMutation;
    const flushInterval = opts.flushInterval || 200;
    const flushSize = opts.flushSize || 50;
    const maxTextLength = opts.maxTextLength || 200;

    let batch = [];
    let flushTimer = null;

    function flush() {
        if (batch.length === 0) return;
        const toSend = batch;
        batch = [];
        onMutation(toSend);
    }

    function scheduleFlush() {
        if (flushTimer !== null) return;
        flushTimer = setTimeout(() => {
            flushTimer = null;
            flush();
        }, flushInterval);
    }

    const observer = new MutationObserver((mutations) => {
        for (const mutation of mutations) {
            if (mutation.type === 'childList') {
                for (const node of mutation.addedNodes) {
                    const el = node.nodeType === Node.ELEMENT_NODE ? node
                             : node.parentElement;
                    if (!el) continue;
                    const bboxes = extractBBoxes({
                        root: el,
                        maxTextLength: maxTextLength,
                        mutationType: "added",
                    });
                    batch.push(...bboxes);
                }
            } else if (mutation.type === 'characterData') {
                const el = mutation.target.parentElement;
                if (!el) continue;
                const bboxes = extractBBoxes({
                    root: el,
                    maxTextLength: maxTextLength,
                    mutationType: "changed",
                });
                batch.push(...bboxes);
            }
        }

        if (batch.length >= flushSize) {
            flush();
        } else if (batch.length > 0) {
            scheduleFlush();
        }
    });

    observer.observe(document.body, {
        childList: true,
        characterData: true,
        subtree: true,
    });

    return function dispose() {
        observer.disconnect();
        if (flushTimer !== null) {
            clearTimeout(flushTimer);
            flushTimer = null;
        }
        flush(); // flush remaining
    };
}
