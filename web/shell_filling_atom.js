/* Illustrative s/p probability clouds at a fixed teaching scale. These are
 * schematic shapes, not the calibrated radial densities of the main viewer. */
(function (root) {
    function electronClouds() {
        let seed = 721;
        const random = () => {
            seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
            return (seed + 0.5) / 4294967296;
        };
        const clouds = [];
        for (const subshell of root.ShellFilling.subshells) {
            for (let electron = 0; electron < subshell.orbitals * 2; ++electron) {
                const axis = electron % subshell.orbitals;
                const points = [];
                while (points.length < 360) {
                    const z = 2 * random() - 1;
                    const phi = 2 * Math.PI * random();
                    const transverse = Math.sqrt(1 - z * z);
                    const direction = [transverse * Math.cos(phi), transverse * Math.sin(phi), z];
                    // Squared p angular amplitude produces the two lobes.
                    if (subshell.orbitals === 3 && random() > direction[axis] ** 2) continue;
                    const radius = subshell.n * (0.32 + 0.68 * Math.cbrt(random()));
                    points.push(direction.map(component => component * radius));
                }
                clouds.push({n: subshell.n, label: subshell.label, axis, points});
            }
        }
        return clouds;
    }

    function createAtomView(canvas, colors) {
        const ctx = canvas.getContext('2d');
        const clouds = electronClouds();
        const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');
        let count = 0;
        let target = 8;
        let angle = 0.4;
        let start = 0;
        let added = false;
        let rotating = true;
        let density = 0.4;
        let motionTime = 0;
        let frame = null;
        let lastTime = null;
        // Cache glow sprites once, rather than creating gradients per point.
        const sprites = [...colors, '#ffe5a3'].map(color => {
            const sprite = document.createElement('canvas');
            sprite.width = sprite.height = 24;
            const brush = sprite.getContext('2d');
            const halo = brush.createRadialGradient(12, 12, 0, 12, 12, 12);
            halo.addColorStop(0, '#ffffff');
            halo.addColorStop(0.12, color);
            halo.addColorStop(0.35, color + '90');
            halo.addColorStop(1, color + '00');
            brush.fillStyle = halo;
            brush.fillRect(0, 0, 24, 24);
            return sprite;
        });
        const retained = (index, epoch) => {
            let hash = Math.imul(index + 1, 1597334677) ^ Math.imul(epoch + 1, 3812015801);
            hash = Math.imul(hash ^ (hash >>> 16), 2246822507);
            return ((hash ^ (hash >>> 13)) >>> 0) / 4294967296 < density;
        };

        function draw(now) {
            frame = null;
            const elapsed = lastTime === null ? 0 : Math.min(now - lastTime, 50);
            lastTime = now;
            if (!reducedMotion.matches) motionTime += elapsed;
            if (rotating && !reducedMotion.matches) angle += elapsed * 0.00035;
            const progress = added && !reducedMotion.matches ? Math.min(1, (now - start) / 450) : 1;
            const size = canvas.width;
            const center = size / 2;
            const outerShell = root.ShellFilling.configuration(target).filter(s => s.occupancy).at(-1).n;
            const scale = size * 0.37 / outerShell;
            ctx.globalCompositeOperation = 'source-over';
            const background = ctx.createRadialGradient(center, center, 0, center, center, center);
            background.addColorStop(0, '#101e35');
            background.addColorStop(1, '#030711');
            ctx.fillStyle = background;
            ctx.fillRect(0, 0, size, size);
            // Slowly exchange the retained subset, without changing occupancy.
            const epoch = reducedMotion.matches ? 0 : Math.floor(motionTime / 1800);
            const blend = reducedMotion.matches ? 1 : Math.min(1, (motionTime % 1800) / 650);
            const cosine = Math.cos(angle);
            const sine = Math.sin(angle);
            const tilt = 0.35 + (reducedMotion.matches ? 0 : 0.15 * Math.sin(angle * 0.46));
            ctx.globalCompositeOperation = 'lighter';
            for (let index = 0; index < count; ++index) {
                const cloud = clouds[index];
                const arriving = added && index === count - 1 && progress < 1;
                const gain = arriving ? progress : 1;
                const expansion = arriving ? 1 + 0.3 * (1 - progress) : 1;
                for (let pointIndex = 0; pointIndex < cloud.points.length; ++pointIndex) {
                    const id = index * 360 + pointIndex;
                    const oldWeight = retained(id, Math.max(0, epoch - 1)) ? 1 : 0;
                    const newWeight = retained(id, epoch) ? 1 : 0;
                    const visibility = oldWeight * (1 - blend) + newWeight * blend;
                    if (!visibility) continue;
                    const [x, y, z] = cloud.points[pointIndex];
                    const rx = x * cosine + z * sine;
                    const rz = z * cosine - x * sine;
                    const ry = y * Math.cos(tilt) - rz * Math.sin(tilt);
                    const depth = y * Math.sin(tilt) + rz * Math.cos(tilt);
                    const near = Math.max(0, Math.min(1, (depth / outerShell + 1) / 2));
                    const diameter = 5 + 3 * near;
                    ctx.globalAlpha = visibility * gain * (0.5 + 0.45 * near);
                    ctx.drawImage(sprites[arriving ? 4 : cloud.n - 1],
                        center + rx * scale * expansion - diameter / 2,
                        center + ry * scale * expansion - diameter / 2, diameter, diameter);
                }
            }
            ctx.globalAlpha = 1;
            ctx.globalCompositeOperation = 'source-over';
            const glow = ctx.createRadialGradient(center, center, 0, center, center, 15);
            glow.addColorStop(0, '#fff4d8');
            glow.addColorStop(0.3, '#ffba72');
            glow.addColorStop(1, '#ffba7200');
            ctx.fillStyle = glow;
            ctx.fillRect(center - 15, center - 15, 30, 30);
            ctx.font = '16px system-ui';
            ctx.fillStyle = '#e9eef8';
            ctx.fillText(`${root.ShellFilling.elements[target - 1][0]} · ${count} / ${target} ${ShellFillingLocale.electrons}`, 18, 28);
            ctx.fillStyle = '#aebbd0';
            ctx.fillText(count ? ShellFillingLocale.latest(clouds[count - 1].label) : ShellFillingLocale.nucleus, 18, size - 18);
            if ((count > 0 || progress < 1) && !reducedMotion.matches && !document.hidden) {
                frame = requestAnimationFrame(draw);
            }
        }

        function refresh() {
            if (frame !== null) cancelAnimationFrame(frame);
            frame = requestAnimationFrame(draw);
        }

        document.addEventListener('visibilitychange', () => {
            lastTime = null;
            if (document.hidden) {
                if (frame !== null) cancelAnimationFrame(frame);
                frame = null;
            } else refresh();
        });
        reducedMotion.addEventListener('change', refresh);
        return {
            update(nextCount, nextTarget, animate) {
                added = animate && nextCount === count + 1 && nextTarget === target;
                count = nextCount;
                target = nextTarget;
                start = performance.now();
                canvas.setAttribute('aria-label', ShellFillingLocale.atomLabel(target, count));
                refresh();
            },
            setRotation(enabled) { rotating = enabled; refresh(); },
            setDensity(percent) { density = Math.max(0.1, Math.min(1, percent / 100)); refresh(); },
        };
    }

    const api = {electronClouds, createAtomView};
    if (typeof module !== 'undefined' && module.exports) module.exports = api;
    else root.ShellFillingAtom = api;
})(globalThis);
