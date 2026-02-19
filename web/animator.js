export class Animator {
    constructor() {
        this.waypoints = [];
        this.isPlaying = false;
        this.currentTime = 0;
        this.totalDuration = 0;
        
        // Settings
        this.defaultTransitionTime = 2.0; // Seconds between waypoints
    }

    addWaypoint(state) {
        // Deep copy the state to prevent mutations
        const wp = {
            view: { ...state.view },
            layers: { ...state.layers },
            timeOffset: this.waypoints.length > 0 ? this.waypoints[this.waypoints.length - 1].timeOffset + this.defaultTransitionTime : 0
        };
        this.waypoints.push(wp);
        this.totalDuration = wp.timeOffset;
    }

    clearWaypoints() {
        this.waypoints = [];
        this.stop();
    }

    play() {
        if (this.waypoints.length < 2) return;
        this.isPlaying = true;
        this.currentTime = 0;
    }

    stop() {
        this.isPlaying = false;
        this.currentTime = 0;
    }

    // Easing function (Cosine / Smoothstep-like)
    easeInOut(t) {
        return 0.5 * (1 - Math.cos(t * Math.PI));
    }

    // Call this every frame with dt (delta time in seconds)
    tick(dt, targetState) {
        if (!this.isPlaying || this.waypoints.length < 2) return false;

        this.currentTime += dt;

        if (this.currentTime >= this.totalDuration) {
            this.currentTime = this.totalDuration;
            this.isPlaying = false; // Stop at the end
            this.applyWaypoint(this.waypoints.length - 1, targetState);
            return true; // Still requires one last render
        }

        // Find the segment we are in
        let idx = 0;
        while (idx < this.waypoints.length - 1 && this.currentTime >= this.waypoints[idx + 1].timeOffset) {
            idx++;
        }

        const wpA = this.waypoints[idx];
        const wpB = this.waypoints[idx + 1];

        // Local time progress between 0.0 and 1.0
        const segmentDuration = wpB.timeOffset - wpA.timeOffset;
        let t = 0;
        if (segmentDuration > 0) {
           t = (this.currentTime - wpA.timeOffset) / segmentDuration;
        }

        // Apply easing
        const easedT = this.easeInOut(t);

        this.interpolate(wpA, wpB, easedT, targetState);
        return true;
    }

    applyWaypoint(idx, targetState) {
        const wp = this.waypoints[idx];
        for (const k in wp.view) {
            targetState.view[k] = wp.view[k];
        }
        for (const lid in wp.layers) {
            if (targetState.layers[lid]) {
                targetState.layers[lid].visible = wp.layers[lid].visible;
            }
        }
    }

    interpolate(wpA, wpB, t, targetState) {
        // Interpolate numeric view parameters
        for (const k in wpA.view) {
            const valA = wpA.view[k];
            const valB = wpB.view[k];
            if (typeof valA === 'number' && typeof valB === 'number') {
                targetState.view[k] = valA + (valB - valA) * t;
            } else if (typeof valA === 'boolean') {
                targetState.view[k] = t < 0.5 ? valA : valB;
            }
        }

        // Interpolate layer opacities. Since layer.visible is boolean,
        // we'll smoothly crossfade if one is false and the other is true.
        // We'll update an .alpha multiplier on the layer config.
        for (const lid in wpA.layers) {
            const visA = wpA.layers[lid].visible;
            const visB = wpB.layers[lid] ? wpB.layers[lid].visible : visA;
            
            if (targetState.layers[lid]) {
                // If both visible, alpha is 1. If both hidden, alpha is 0.
                // If fading in (visA=false, visB=true), alpha goes 0 -> 1.
                // If fading out (visA=true, visB=false), alpha goes 1 -> 0.
                let alpha = 1.0;
                if (!visA && !visB) alpha = 0.0;
                else if (!visA && visB) alpha = t;
                else if (visA && !visB) alpha = 1.0 - t;

                targetState.layers[lid].alphaMultiplier = alpha;
                
                // Keep the layer rendered as long as alpha > 0
                targetState.layers[lid].visible = (alpha > 0);
            }
        }
    }
}
