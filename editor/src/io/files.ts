// Local files through standard APIs both Chrome and Firefox implement: <input type=file> to
// open, a Blob download to save (where it lands is the browser's download setting).

export { default as projectPresets } from '../../../presets.toml?raw';

export function openTomlFile(): Promise<{ text: string; name: string } | null> {
  const { promise, resolve, reject } = Promise.withResolvers<{ text: string; name: string } | null>();
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = '.toml,text/plain';
  input.style.display = 'none';
  input.addEventListener('change', () => {
    const file = input.files?.[0];
    input.remove();
    if (!file) return resolve(null);
    file.text().then((text) => resolve({ text, name: file.name }), reject);
  });
  input.addEventListener('cancel', () => {
    input.remove();
    resolve(null);
  });
  document.body.append(input);
  input.click();
  return promise;
}

export function saveTomlFile(text: string, name: string): void {
  const url = URL.createObjectURL(new Blob([text], { type: 'text/plain' }));
  const a = document.createElement('a');
  a.href = url;
  a.download = name;
  document.body.append(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(url);
}
