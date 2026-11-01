import re

def function(source, name):
    # Preserve offsets while hiding braces in comments/strings/character literals.
    lexical = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                     lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), source)
    match = re.search(r'(?m)^'+re.escape(name)+r'\([^;]*?\)\n\{', lexical)
    if not match:
        raise ValueError('missing function '+name)
    opening = lexical.index('{', match.start())
    depth = 1
    end = opening+1
    while depth:
        depth += (lexical[end] == '{')-(lexical[end] == '}')
        end += 1
    return source[match.start():end]+'\n'
