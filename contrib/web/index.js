const fs = require('fs');
const marked = require('marked');

// files to read and process
const files = [
  'README.md',
  'BUILDING.md',
  'CONFIGURATION.md',
  'FUNCTIONS.md',
  'BUILTINS.md',
  'EXTERNAL.md'
];

// template
var template = fs.readFileSync('./index.tpl', 'utf8');

// links
var links = [ ];

// Override function
const renderer = {
  heading (text, level) {
    const escapedText = text.toLowerCase().replace(/[^\w]+/g, '-');

    links.push({
      level: level,
      link: '#' + escapedText,
      text: text
    });

    return `
      <h${level}>
        <a name="${escapedText}" class="anchor" href="#${escapedText}">
          <span class="header-link"></span>
        </a>
        ${text}
      </h${level}>
    `;
  }
};

marked.use({ renderer });

// create the content
var content = '';

files.forEach((elem) => {
  const input = fs.readFileSync('../../docs/' + elem, 'utf8');

  content += marked.parse(input);
});

// replace the content of the template
template = template.replace('%CONTENT%', () => content);

// create the links for the sidebar
var level = 0;

var sidebar = '';

links.forEach((elem) => {
  if (elem.level > level) {
    level++;
    sidebar += '<ul>\n';
  } else if (elem.level < level) {
    level--;
    sidebar += '</ul>\n';
  }

  sidebar += `<li><a href="${elem.link}" class="anchor">${elem.text}</li>\n`;
});

// replace the content of the sidebar
template = template.replace('%SIDEBAR%', () => sidebar);

// write the output
fs.writeFileSync('index.html', template, 'utf8');
