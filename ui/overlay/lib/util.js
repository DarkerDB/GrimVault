export function distance (p1, p2) {
  return Math.sqrt (Math.pow (p2.x - p1.x, 2) + Math.pow (p2.y - p1.y, 2));
}

export function interpolateColor (value, steps, from, to) {
  from = from || [ 255, 0, 0 ];
  to = to || [ 0, 255, 0 ];

  const results = [];
  
  for (let i = 0; i < steps; i++) {
    const t = i / (steps - 1);

    const r = Math.round (from [0] + (to [0] - from [0]) * t);
    const g = Math.round (from [1] + (to [1] - from [1]) * t);
    const b = Math.round (from [2] + (to [2] - from [2]) * t);
    
    results.push (`rgb(${r}, ${g}, ${b})`);
  }

  return results [value - 1];
}