export function createStore(initialState) {
    let state = { ...initialState };
    const listeners = new Set();
    const keyListeners = new Map(); 

    const notify = (prevState) => {
        listeners.forEach(fn => fn(state, prevState));
        for (const [key, fns] of keyListeners.entries()) {
            if (state[key] !== prevState[key]) {
                fns.forEach(fn => fn(state[key], prevState[key]));
            }
        }
    };

    return {
        get() { return state; },
        set(newState) {
            const prevState = { ...state };
            let changed = false;
            for (const key in newState) {
                if (state[key] !== newState[key]) {
                    state[key] = newState[key];
                    changed = true;
                }
            }
            if (changed) notify(prevState);
        },
        subscribe(fn) {
            listeners.add(fn);
            fn(state, state); 
            return () => listeners.delete(fn);
        },
        // Subscribe to a specific slice/key of the state
        bind(key, fn) {
            if (!keyListeners.has(key)) keyListeners.set(key, new Set());
            keyListeners.get(key).add(fn);
            fn(state[key], state[key]);
            return () => keyListeners.get(key).delete(fn);
        }
    };
}
