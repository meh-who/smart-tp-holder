// Canned text used in demo mode (DEMO=1), so the installation can be rehearsed
// without API credentials or a network connection. All lines are invented.
export const demoLines = {
  news: [
    'City council votes to extend the late-night bus pilot through spring.',
    'Markets close flat after a week of mixed earnings reports.',
    'Researchers report a new method for recycling mixed plastics.',
    'Heavy rain expected across the region through Tuesday.',
    'Museum attendance returns to pre-2020 levels, survey finds.',
  ],
  x: [
    'hot take: breakfast is just dinner with better PR',
    'nobody talks about how good the light is at 4pm in october',
    'shipping it friday. what could go wrong',
    'thread on why everything you know about sourdough is wrong 1/14',
    'me: I will go to bed early. also me at 1am: reading about medieval bridges',
  ],
  tiktok: [
    'day 3 of trying every snack at the corner store',
    'POV: you finally clean your desk',
    'replying to comments: yes the cat is fine',
    'get ready with me for absolutely nothing',
    'this one trick changed how I fold towels',
  ],
  instagram: [
    'golden hour never misses',
    'weekend in three seconds',
    'new studio, same chaos',
    'the before and after nobody asked for',
    'swipe for the blooper',
  ],
};

export function demoItems(source) {
  return (demoLines[source] || []).map((text, i) => ({ id: `demo-${source}-${i}`, text }));
}
