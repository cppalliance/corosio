/* GCOVR Custom JavaScript - Tree View & Interactivity */

(function() {
  'use strict';

  // Wait for DOM ready
  document.addEventListener('DOMContentLoaded', function() {
    initTheme();
    initSidebar();
    initSidebarResize();
    initMobileMenu();
    initFileTree();
    initNavOverride();
    initBreadcrumbs();
    initSearch();
    initFunctionRows();
    initSorting();
    initToggleButtons();
    initCoverageNav();
    initTreeControls();
    initViewToggle();
    initSettingsDropdown();
    initTlaNavigation();
    initLineHighlight();
    initColumnToggles();
    initPopupResize();
    initFileNavTooltips();
    initFileNavKeys();
    initFunctionListPersistence();

    // Reveal page now that all init is done
    document.documentElement.classList.remove('no-transitions');

    // Prefetch linked pages on hover for instant navigation
    initPrefetch();
  });

  // ===========================================
  // Breadcrumb Links
  // ===========================================

  // Find a node in the tree by its link (HTML filename) and return
  // the full ancestor path as an array of nodes from root to target.
  function findPathInTree(nodes, targetLink) {
    for (var i = 0; i < nodes.length; i++) {
      var node = nodes[i];
      if (node.link === targetLink) {
        return [node];
      }
      if (node.children) {
        var childPath = findPathInTree(node.children, targetLink);
        if (childPath) {
          return [node].concat(childPath);
        }
      }
    }
    return null;
  }

  function initBreadcrumbs() {
    var currentSpan = document.querySelector('.breadcrumb .current');
    if (!currentSpan || !window.GCOVR_TREE_DATA) {
      if (currentSpan) currentSpan.classList.add('ready');
      return;
    }

    // Find current page in tree by its HTML filename — this is unambiguous
    // since each page only appears once in the tree.
    var currentPage = window.location.pathname.split('/').pop() || 'index.html';
    var treePath = findPathInTree(window.GCOVR_TREE_DATA, currentPage);

    if (!treePath || treePath.length === 0) {
      currentSpan.classList.add('ready');
      return;
    }

    // Build breadcrumb from the tree path (ancestor nodes → current node)
    var fragment = document.createDocumentFragment();
    var matchedSegments = [];

    // Fill an element with the segments of a (possibly joined) name like
    // "boost/url", rendering "boost", a separator, "url". Used so a joined
    // directory shows its segments inline yet remains one hyperlink target.
    function appendSegments(parentEl, name) {
      var segments = name.split('/');
      for (var k = 0; k < segments.length; k++) {
        if (k > 0) {
          var inner = document.createElement('span');
          inner.className = 'separator';
          inner.textContent = '/';
          parentEl.appendChild(inner);
        }
        parentEl.appendChild(document.createTextNode(segments[k]));
      }
    }

    for (var i = 0; i < treePath.length; i++) {
      var node = treePath[i];
      var isLast = (i === treePath.length - 1);

      if (i > 0) {
        var sep = document.createElement('span');
        sep.className = 'separator';
        sep.textContent = '/';
        fragment.appendChild(sep);
      }

      matchedSegments.push(node.name);

      if (node.link && !isLast) {
        var a = document.createElement('a');
        a.href = node.link;
        appendSegments(a, node.name);
        fragment.appendChild(a);
      } else {
        var span = document.createElement('span');
        span.className = 'current-file';
        appendSegments(span, node.name);
        fragment.appendChild(span);
      }
    }

    currentSpan.innerHTML = '';
    currentSpan.appendChild(fragment);
    currentSpan.classList.add('ready');

    // Update source-filename to match breadcrumb path
    var sourceFilename = document.querySelector('.source-filename');
    if (sourceFilename) {
      sourceFilename.textContent = matchedSegments.join('/');
    }
  }

  // ===========================================
  // Theme Toggle
  // ===========================================

  function initTheme() {
    const toggle = document.getElementById('theme-toggle');
    const iconSun = toggle ? toggle.querySelector('.icon-sun') : null;
    const iconMoon = toggle ? toggle.querySelector('.icon-moon') : null;

    // Get system preference
    function getSystemTheme() {
      return window.matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark';
    }

    // Get effective theme: saved preference or OS default
    function getEffectiveTheme() {
      var saved = localStorage.getItem('gcovr-theme');
      return (saved === 'light' || saved === 'dark') ? saved : getSystemTheme();
    }

    // Apply theme to document
    function applyTheme(theme) {
      document.documentElement.setAttribute('data-theme', theme);
      if (iconSun) iconSun.style.display = (theme === 'dark') ? 'block' : 'none';
      if (iconMoon) iconMoon.style.display = (theme === 'light') ? 'block' : 'none';
    }

    // Apply current theme
    applyTheme(getEffectiveTheme());

    // Listen for system theme changes — only apply if no stored preference
    window.matchMedia('(prefers-color-scheme: light)').addEventListener('change', function() {
      var saved = localStorage.getItem('gcovr-theme');
      if (saved !== 'light' && saved !== 'dark') {
        applyTheme(getSystemTheme());
      }
    });

    // Toggle between light and dark on click
    if (toggle) {
      toggle.addEventListener('click', function() {
        var current = getEffectiveTheme();
        var next = (current === 'dark') ? 'light' : 'dark';
        localStorage.setItem('gcovr-theme', next);
        applyTheme(next);
      });
    }
  }

  // ===========================================
  // Tree Controls (Expand/Collapse All)
  // ===========================================

  function initTreeControls() {
    var expandBtn = document.getElementById('expand-all');
    var collapseBtn = document.getElementById('collapse-all');

    if (expandBtn) {
      expandBtn.addEventListener('click', function() {
        document.querySelectorAll('.tree-item').forEach(function(item) {
          if (!item.classList.contains('no-children')) {
            item.classList.add('expanded');
            var toggle = item.querySelector(':scope > .tree-item-header > .tree-folder-toggle');
            if (toggle) toggle.textContent = '−';
          }
        });
        saveExpandedFolders();
      });
    }

    if (collapseBtn) {
      collapseBtn.addEventListener('click', function() {
        document.querySelectorAll('.tree-item').forEach(function(item) {
          item.classList.remove('expanded');
          var toggle = item.querySelector(':scope > .tree-item-header > .tree-folder-toggle');
          if (toggle) toggle.textContent = '+';
        });
        saveExpandedFolders();
      });
    }
  }

  // ===========================================
  // Sidebar Toggle
  // ===========================================

  function initSidebar() {
    const sidebar = document.getElementById('sidebar');
    const toggle = document.getElementById('sidebar-toggle');
    const header = sidebar ? sidebar.querySelector('.sidebar-header') : null;

    if (!sidebar) return;

    // Load saved state
    const isCollapsed = localStorage.getItem('sidebar-collapsed') === 'true';
    if (isCollapsed) {
      sidebar.classList.add('collapsed');
    }

    // Toggle button
    if (toggle) {
      toggle.addEventListener('click', function() {
        sidebar.classList.toggle('collapsed');
        sidebar.classList.remove('hover-expand');
        var isNowCollapsed = sidebar.classList.contains('collapsed');
        localStorage.setItem('sidebar-collapsed', isNowCollapsed);
        // Restore custom width when un-collapsing
        if (!isNowCollapsed) {
          var savedWidth = localStorage.getItem('gcovr-sidebar-width');
          if (savedWidth) {
            document.documentElement.style.setProperty('--sidebar-width', savedWidth + 'px');
          }
        }
      });
    }

    // Hover expand - expands when hovering sidebar content (not header or no-expand zones)
    var hoverTimeout = null;
    var HOVER_DELAY = 150; // ms delay before expanding
    var isOverContent = false;

    // Check if element is within a no-expand zone
    function isInNoExpandZone(el) {
      while (el && el !== sidebar) {
        if (el.classList && el.classList.contains('no-expand')) {
          return true;
        }
        el = el.parentElement;
      }
      return false;
    }

    function scheduleExpand() {
      if (hoverTimeout) return; // already scheduled
      if (sidebar.classList.contains('hover-expand')) return; // already expanded
      hoverTimeout = setTimeout(function() {
        if (isOverContent) {
          sidebar.classList.add('hover-expand');
        }
        hoverTimeout = null;
      }, HOVER_DELAY);
    }

    function cancelExpand() {
      if (hoverTimeout) {
        clearTimeout(hoverTimeout);
        hoverTimeout = null;
      }
      sidebar.classList.remove('hover-expand');
    }

    sidebar.addEventListener('mouseenter', function(e) {
      if (!sidebar.classList.contains('collapsed')) return;
      // Check if entering over content area (not header or no-expand zones)
      if (!header.contains(e.target) && !isInNoExpandZone(e.target)) {
        isOverContent = true;
        scheduleExpand();
      }
    });

    sidebar.addEventListener('mousemove', function(e) {
      if (!sidebar.classList.contains('collapsed')) return;
      var wasOverContent = isOverContent;
      isOverContent = !header.contains(e.target) && !isInNoExpandZone(e.target);

      if (isOverContent && !wasOverContent && !sidebar.classList.contains('hover-expand')) {
        scheduleExpand();
      }
    });

    sidebar.addEventListener('mouseleave', function() {
      isOverContent = false;
      cancelExpand();
    });
  }

  // ===========================================
  // Sidebar Resize
  // ===========================================

  function initSidebarResize() {
    var sidebar = document.getElementById('sidebar');
    var handle = document.getElementById('sidebar-resize-handle');
    if (!sidebar || !handle) return;

    var MIN_WIDTH = 200;
    var startX, startWidth;

    // Restore saved width
    var savedWidth = localStorage.getItem('gcovr-sidebar-width');
    if (savedWidth && !sidebar.classList.contains('collapsed')) {
      var w = parseInt(savedWidth, 10);
      if (w >= MIN_WIDTH) {
        document.documentElement.style.setProperty('--sidebar-width', w + 'px');
      }
    }

    function getMaxWidth() {
      return Math.floor(window.innerWidth * 0.5);
    }

    function onMouseMove(e) {
      var newWidth = startWidth + (e.clientX - startX);
      var maxW = getMaxWidth();
      if (newWidth < MIN_WIDTH) newWidth = MIN_WIDTH;
      if (newWidth > maxW) newWidth = maxW;
      document.documentElement.style.setProperty('--sidebar-width', newWidth + 'px');
    }

    function onMouseUp() {
      document.body.classList.remove('sidebar-resizing');
      document.removeEventListener('mousemove', onMouseMove);
      document.removeEventListener('mouseup', onMouseUp);
      // Save the current width
      var computed = parseInt(getComputedStyle(sidebar).width, 10);
      localStorage.setItem('gcovr-sidebar-width', computed);
    }

    handle.addEventListener('mousedown', function(e) {
      if (sidebar.classList.contains('collapsed')) return;
      e.preventDefault();
      startX = e.clientX;
      startWidth = parseInt(getComputedStyle(sidebar).width, 10);
      document.body.classList.add('sidebar-resizing');
      document.addEventListener('mousemove', onMouseMove);
      document.addEventListener('mouseup', onMouseUp);
    });

    // Double-click to reset to default width
    var DEFAULT_WIDTH = 320;
    handle.addEventListener('dblclick', function() {
      if (sidebar.classList.contains('collapsed')) return;
      document.documentElement.style.setProperty('--sidebar-width', DEFAULT_WIDTH + 'px');
      localStorage.setItem('gcovr-sidebar-width', DEFAULT_WIDTH);
    });
  }

  // ===========================================
  // Mobile Menu
  // ===========================================

  function initMobileMenu() {
    var sidebar = document.getElementById('sidebar');
    var menuBtn = document.getElementById('mobile-menu-btn');
    var backdrop = document.getElementById('sidebar-backdrop');

    if (!menuBtn || !sidebar) return;

    // Open sidebar on hamburger click
    menuBtn.addEventListener('click', function() {
      sidebar.classList.add('mobile-open');
    });

    // Close on backdrop click
    if (backdrop) {
      backdrop.addEventListener('click', function() {
        sidebar.classList.remove('mobile-open');
      });
    }

    // Close when clicking a navigation link
    sidebar.addEventListener('click', function(e) {
      if (e.target.closest('a[href]')) {
        sidebar.classList.remove('mobile-open');
      }
    });

    // Close on escape key
    document.addEventListener('keydown', function(e) {
      if (e.key === 'Escape' && sidebar.classList.contains('mobile-open')) {
        sidebar.classList.remove('mobile-open');
      }
    });
  }

  // ===========================================
  // File Tree - Load from tree.json
  // ===========================================

  function initFileTree() {
    var treeContainer = document.getElementById('file-tree');
    if (!treeContainer) return;

    renderTree(treeContainer, window.GCOVR_TREE_DATA);
  }

  // Save expanded folder paths to localStorage
  function saveExpandedFolders() {
    var paths = [];
    document.querySelectorAll('.tree-item.expanded[data-tree-path]').forEach(function(el) {
      paths.push(el.getAttribute('data-tree-path'));
    });
    localStorage.setItem('gcovr-expanded-folders', JSON.stringify(paths));
  }

  function renderTree(container, tree) {
    container.innerHTML = '';

    if (!tree || tree.length === 0) {
      container.innerHTML = '<div class="tree-loading">No files found</div>';
      return;
    }

    tree.forEach(function(item) {
      container.appendChild(createTreeItem(item, ''));
    });

    // Auto-expand to current file and highlight it
    expandToCurrentFile(container);
  }

  function expandToCurrentFile(container) {
    // Get current page filename
    var currentPage = window.location.pathname.split('/').pop() || 'index.html';

    // Find the link matching current page
    var currentLink = container.querySelector('a[href="' + currentPage + '"]');

    if (currentLink) {
      // Mark as active
      var treeItem = currentLink.closest('.tree-item');
      if (treeItem) {
        treeItem.classList.add('active');
      }

      // Expand all parent folders
      var parent = currentLink.closest('.tree-children');
      while (parent) {
        var parentItem = parent.closest('.tree-item');
        if (parentItem) {
          parentItem.classList.add('expanded');
          var toggle = parentItem.querySelector(':scope > .tree-item-header > .tree-folder-toggle');
          if (toggle) toggle.textContent = '−';
        }
        parent = parentItem ? parentItem.parentElement.closest('.tree-children') : null;
      }
    }

    // Restore previously expanded folders from localStorage
    try {
      var saved = localStorage.getItem('gcovr-expanded-folders');
      if (saved) {
        var paths = JSON.parse(saved);
        paths.forEach(function(path) {
          var el = container.querySelector('.tree-item[data-tree-path="' + CSS.escape(path) + '"]');
          if (el && !el.classList.contains('no-children')) {
            el.classList.add('expanded');
            var toggle = el.querySelector(':scope > .tree-item-header > .tree-folder-toggle');
            if (toggle) toggle.textContent = '−';
          }
        });
      }
    } catch (e) {
      // Ignore localStorage errors
    }

    // Scroll active item into view instantly
    if (currentLink) {
      currentLink.scrollIntoView({ block: 'center', behavior: 'instant' });
    }
  }

  // Clean relative path prefixes like '../../../' from names
  function cleanPathName(name) {
    if (!name) return 'unknown';
    // Remove leading ./ or ../
    while (name.indexOf('./') === 0 || name.indexOf('../') === 0) {
      if (name.indexOf('./') === 0) {
        name = name.substring(2);
      } else if (name.indexOf('../') === 0) {
        name = name.substring(3);
      }
    }
    return name || 'unknown';
  }

  // Get just the filename from a path
  function getDisplayName(name) {
    var cleaned = cleanPathName(name);
    var lastSlash = cleaned.lastIndexOf('/');
    return lastSlash >= 0 ? cleaned.substring(lastSlash + 1) : cleaned;
  }

  function createTreeItem(item, parentPath) {
    var hasChildren = item.children && item.children.length > 0;
    var isDirectory = item.isDirectory || hasChildren;
    var cleanedName = cleanPathName(item.name);
    var treePath = parentPath ? (parentPath + '/' + cleanedName) : cleanedName;

    var div = document.createElement('div');
    div.className = 'tree-item' + (isDirectory ? ' is-folder' : '') + (hasChildren ? '' : ' no-children');
    div.setAttribute('data-tree-path', treePath);

    var header = document.createElement('div');
    header.className = 'tree-item-header';
    var toggle = null;

    // Toggle button (+/-) for folders with children
    if (hasChildren) {
      toggle = document.createElement('button');
      toggle.className = 'tree-folder-toggle';
      toggle.textContent = '+';
      toggle.setAttribute('aria-label', 'Toggle folder');
      toggle.addEventListener('click', function(e) {
        e.stopPropagation();
        e.preventDefault();
        var isExpanded = div.classList.toggle('expanded');
        toggle.textContent = isExpanded ? '−' : '+';
        saveExpandedFolders();
      });
      header.appendChild(toggle);

      // Make entire header clickable to expand/collapse
      header.style.cursor = 'pointer';
      header.addEventListener('click', function(e) {
        // If clicking directly on a link, let it navigate
        if (e.target.closest('a')) return;
        e.preventDefault();
        var isExpanded = div.classList.toggle('expanded');
        toggle.textContent = isExpanded ? '−' : '+';
        saveExpandedFolders();
      });
    } else {
      var spacer = document.createElement('span');
      spacer.className = 'tree-spacer';
      header.appendChild(spacer);
    }

    // Icon - different for folders vs files
    var icon = document.createElement('span');
    if (isDirectory) {
      icon.className = 'tree-icon tree-icon-folder';
      icon.innerHTML = '<svg viewBox="0 0 16 16" width="16" height="16"><path fill="currentColor" d="M1.75 1A1.75 1.75 0 000 2.75v10.5C0 14.216.784 15 1.75 15h12.5A1.75 1.75 0 0016 13.25v-8.5A1.75 1.75 0 0014.25 3H7.5a.25.25 0 01-.2-.1l-.9-1.2C6.07 1.26 5.55 1 5 1H1.75z"/></svg>';
    } else {
      icon.className = 'tree-icon tree-icon-file';
      icon.innerHTML = '<svg viewBox="0 0 16 16" width="16" height="16"><path fill="currentColor" d="M3.75 1.5a.25.25 0 00-.25.25v12.5c0 .138.112.25.25.25h9.5a.25.25 0 00.25-.25V6h-2.75A1.75 1.75 0 019 4.25V1.5H3.75zm6.75.062V4.25c0 .138.112.25.25.25h2.688l-2.938-2.938zM2 1.75C2 .784 2.784 0 3.75 0h6.586c.464 0 .909.184 1.237.513l2.914 2.914c.329.328.513.773.513 1.237v9.586A1.75 1.75 0 0113.25 16h-9.5A1.75 1.75 0 012 14.25V1.75z"/></svg>';
    }
    header.appendChild(icon);

    // Label (with link if available)
    // Use the full cleaned name so joined-dir entries like "boost/url"
    // display as a single multi-segment label in the sidebar.
    var displayName = cleanPathName(item.name);
    var tooltipText = cleanPathName(item.fullPath || item.name);
    var label = document.createElement('span');
    label.className = 'tree-label';
    label.title = tooltipText;
    if (item.link) {
      var link = document.createElement('a');
      link.href = item.link;
      link.textContent = displayName;
      link.title = tooltipText;
      label.appendChild(link);
    } else {
      label.textContent = displayName;
    }
    header.appendChild(label);

    div.appendChild(header);

    // Children container (for expand/collapse)
    if (hasChildren) {
      var childrenWrapper = document.createElement('div');
      childrenWrapper.className = 'tree-children';

      var childrenInner = document.createElement('div');
      childrenInner.className = 'tree-children-inner';
      item.children.forEach(function(child) {
        childrenInner.appendChild(createTreeItem(child, treePath));
      });

      childrenWrapper.appendChild(childrenInner);
      div.appendChild(childrenWrapper);
    }

    return div;
  }

  // ===========================================
  // Search
  // ===========================================

  function initSearch() {
    const searchInput = document.getElementById('file-search');
    const fileTree = document.getElementById('file-tree');
    const clearBtn = document.getElementById('search-clear');
    const searchContainer = searchInput ? searchInput.closest('.sidebar-search') : null;
    if (!searchInput || !fileTree) return;

    // Store pre-search expanded state so we can restore it
    var preSearchExpanded = null;

    // Create no-results message
    var noResults = document.createElement('div');
    noResults.className = 'search-no-results';
    noResults.textContent = 'No matching files';
    noResults.style.display = 'none';
    fileTree.appendChild(noResults);

    function updateClearButton() {
      if (searchContainer) {
        searchContainer.classList.toggle('has-query', searchInput.value.trim() !== '');
      }
    }

    // Clear button
    if (clearBtn) {
      clearBtn.addEventListener('click', function() {
        searchInput.value = '';
        sessionStorage.removeItem('gcovr-search');
        updateClearButton();
        performSearch('');
        searchInput.focus();
      });
    }

    var debounceTimer = null;
    searchInput.addEventListener('input', function() {
      updateClearButton();
      clearTimeout(debounceTimer);
      debounceTimer = setTimeout(function() {
        var val = searchInput.value;
        if (val.trim() !== '') {
          sessionStorage.setItem('gcovr-search', val);
        } else {
          sessionStorage.removeItem('gcovr-search');
        }
        performSearch(val);
      }, 150);
    });

    // Restore search state from sessionStorage on page load (synchronous
    // since initFileTree has already built the tree before initSearch runs)
    var savedSearch = sessionStorage.getItem('gcovr-search');
    if (savedSearch) {
      searchInput.value = savedSearch;
      updateClearButton();
      performSearch(savedSearch);
    }

    function performSearch(value) {
      var query = value.toLowerCase().trim();
      var allItems = fileTree.querySelectorAll('.tree-item');

      // Clear all highlights
      fileTree.querySelectorAll('.search-highlight').forEach(function(mark) {
        var parent = mark.parentNode;
        parent.replaceChild(document.createTextNode(mark.textContent), mark);
        parent.normalize();
      });

      // If query is empty, restore original state
      if (query === '') {
        noResults.style.display = 'none';
        allItems.forEach(function(item) {
          item.style.display = '';
          item.classList.remove('search-match');
        });
        // Restore pre-search expanded state
        if (preSearchExpanded !== null) {
          allItems.forEach(function(item) {
            var path = item.getAttribute('data-tree-path');
            var toggle = item.querySelector(':scope > .tree-item-header > .tree-folder-toggle');
            if (toggle) {
              if (preSearchExpanded.indexOf(path) >= 0) {
                item.classList.add('expanded');
                toggle.textContent = '\u2212';
              } else {
                item.classList.remove('expanded');
                toggle.textContent = '+';
              }
            }
          });
          preSearchExpanded = null;
        }
        return;
      }

      // Save expanded state before first search
      if (preSearchExpanded === null) {
        preSearchExpanded = [];
        allItems.forEach(function(item) {
          if (item.classList.contains('expanded')) {
            preSearchExpanded.push(item.getAttribute('data-tree-path'));
          }
        });
      }

      // Determine which items match (check full path and display name)
      var matchSet = new Set();

      allItems.forEach(function(item) {
        var path = (item.getAttribute('data-tree-path') || '').toLowerCase();
        var label = item.querySelector(':scope > .tree-item-header > .tree-label');
        var text = label ? label.textContent.toLowerCase() : '';
        if (path.includes(query) || text.includes(query)) {
          matchSet.add(item);
        }
      });

      // Also mark all ancestor items of matches as visible
      var visibleSet = new Set(matchSet);
      matchSet.forEach(function(item) {
        var parent = item.parentElement;
        while (parent && parent !== fileTree) {
          if (parent.classList && parent.classList.contains('tree-item')) {
            visibleSet.add(parent);
          }
          parent = parent.parentElement;
        }
      });

      // Apply visibility, expand parents of matches, highlight text
      var anyVisible = false;
      allItems.forEach(function(item) {
        var isVisible = visibleSet.has(item);
        item.style.display = isVisible ? '' : 'none';
        item.classList.toggle('search-match', matchSet.has(item));

        if (isVisible) {
          anyVisible = true;
          // Auto-expand folders that contain matches
          var toggle = item.querySelector(':scope > .tree-item-header > .tree-folder-toggle');
          if (toggle && visibleSet.has(item) && !matchSet.has(item) || (toggle && matchSet.has(item) && item.classList.contains('is-folder'))) {
            item.classList.add('expanded');
            toggle.textContent = '\u2212';
          }
        }

        // Highlight matched text in label
        if (matchSet.has(item)) {
          var label = item.querySelector(':scope > .tree-item-header > .tree-label');
          if (label) {
            highlightText(label, query);
          }
        }
      });

      noResults.style.display = anyVisible ? 'none' : '';
    }

    function highlightText(container, query) {
      // Walk text nodes inside the label (may be inside an <a> tag)
      var walker = document.createTreeWalker(container, NodeFilter.SHOW_TEXT, null, false);
      var textNodes = [];
      while (walker.nextNode()) {
        textNodes.push(walker.currentNode);
      }
      textNodes.forEach(function(node) {
        var text = node.textContent;
        var lowerText = text.toLowerCase();
        var idx = lowerText.indexOf(query);
        if (idx === -1) return;

        var frag = document.createDocumentFragment();
        var lastIdx = 0;
        while (idx !== -1) {
          if (idx > lastIdx) {
            frag.appendChild(document.createTextNode(text.substring(lastIdx, idx)));
          }
          var mark = document.createElement('mark');
          mark.className = 'search-highlight';
          mark.textContent = text.substring(idx, idx + query.length);
          frag.appendChild(mark);
          lastIdx = idx + query.length;
          idx = lowerText.indexOf(query, lastIdx);
        }
        if (lastIdx < text.length) {
          frag.appendChild(document.createTextNode(text.substring(lastIdx)));
        }
        node.parentNode.replaceChild(frag, node);
      });
    }
  }

  // ===========================================
  // Progressive Function Row Rendering
  // ===========================================

  function initFunctionRows() {
    var dataEl = document.getElementById('functions-data');
    if (!dataEl) return;

    var config = window.__functionsPageConfig || {};
    var data = JSON.parse(dataEl.textContent);
    var container = document.querySelector('.functions-body');
    var loadingEl = document.getElementById('functions-loading');
    var showBranches = config.showBranches;
    var showConditions = config.showConditions;
    var singlePage = config.singlePage;
    var currentFile = config.htmlFilename || '';

    if (data.length === 0) {
      if (loadingEl) loadingEl.remove();
      return;
    }

    // --- Virtual scrolling setup ---
    var ROW_HEIGHT = 52;
    var BUFFER = 10;
    var visibleCount = Math.max(30, Math.ceil(container.clientHeight / ROW_HEIGHT) + BUFFER * 2);
    var viewport, visibleEl;
    var lastStartIdx = -1;

    window.addEventListener('resize', function() {
      visibleCount = Math.max(30, Math.ceil(container.clientHeight / ROW_HEIGHT) + BUFFER * 2);
      lastStartIdx = -1;
      renderVisible();
    });

    function buildHref(entry) {
      if (singlePage) return '#' + entry.html_filename + '|l' + entry.line;
      if (currentFile !== entry.html_filename) return entry.html_filename + '#l' + entry.line;
      return '#l' + entry.line;
    }

    function entryKey(entry) {
      return entry.name + '|' + entry.filename + ':' + entry.line;
    }

    function el(tag, cls, text) {
      var node = document.createElement(tag);
      if (cls) node.className = cls;
      if (text !== undefined) node.textContent = text;
      return node;
    }

    function createRow(entry) {
      var row = el('div', 'function-row');
      if (highlightKey && entryKey(entry) === highlightKey) {
        row.classList.add('function-row-visited');
      }

      // col-function
      var colFn = el('div', 'col-function');
      var a = document.createElement('a');
      a.href = buildHref(entry);
      a.appendChild(el('span', 'function-name', entry.name));
      a.appendChild(el('span', 'function-location', entry.filename + ':' + entry.line));
      colFn.appendChild(a);
      row.appendChild(colFn);

      // col-calls
      var colCalls = el('div', 'col-calls');
      var callSpan;
      if (entry.excluded) {
        callSpan = el('span', 'excluded', 'excluded');
      } else if (entry.execution_count === 0) {
        callSpan = el('span', 'not-called', 'not called');
      } else {
        callSpan = el('span', 'called', entry.execution_count + 'x');
      }
      colCalls.appendChild(callSpan);
      row.appendChild(colCalls);

      // col-lines
      row.appendChild(el('div', 'col-lines', entry.line_coverage + '%'));

      // col-branches (optional)
      if (showBranches) {
        row.appendChild(el('div', 'col-branches', entry.branch_coverage + '%'));
      }

      // col-conditions (optional)
      if (showConditions) {
        row.appendChild(el('div', 'col-conditions', entry.condition_coverage + '%'));
      }

      return row;
    }

    function setupVirtualScroll() {
      if (loadingEl) loadingEl.remove();

      viewport = document.createElement('div');
      viewport.className = 'functions-viewport';
      viewport.style.height = (data.length * ROW_HEIGHT) + 'px';

      visibleEl = document.createElement('div');
      visibleEl.className = 'functions-visible';

      viewport.appendChild(visibleEl);
      container.appendChild(viewport);
    }

    function renderVisible() {
      var scrollTop = container.scrollTop;
      var startIdx = Math.max(0, Math.floor(scrollTop / ROW_HEIGHT) - BUFFER);
      var endIdx = Math.min(data.length, startIdx + visibleCount + BUFFER);

      // Skip re-render if the window hasn't shifted
      if (startIdx === lastStartIdx) return;
      lastStartIdx = startIdx;

      visibleEl.style.top = (startIdx * ROW_HEIGHT) + 'px';

      var frag = document.createDocumentFragment();
      for (var i = startIdx; i < endIdx; i++) {
        frag.appendChild(createRow(data[i]));
      }
      visibleEl.replaceChildren(frag);
    }

    // --- Scroll listener (rAF-throttled) ---
    var ticking = false;
    container.addEventListener('scroll', function() {
      if (!ticking) {
        requestAnimationFrame(function() { renderVisible(); ticking = false; });
        ticking = true;
      }
    });

    // --- Save state on navigation for back-button restore ---
    var highlightKey = null;
    container.addEventListener('click', function(e) {
      var row = e.target.closest('.function-row');
      if (!row) return;
      var link = row.querySelector('a');
      if (!link) return;
      var nameEl = row.querySelector('.function-name');
      var locEl = row.querySelector('.function-location');
      if (nameEl && locEl) {
        sessionStorage.setItem('gcovr-functions-clicked', nameEl.textContent + '|' + locEl.textContent);
      }
      sessionStorage.setItem('gcovr-functions-scrollTop', String(container.scrollTop));
    });

    // --- Data-level sorting ---
    function sortData(key, ascending) {
      data.sort(function(a, b) {
        var aVal, bVal;
        switch (key) {
          case 'name': aVal = a.name; bVal = b.name; break;
          case 'calls': aVal = a.excluded ? -1 : a.execution_count; bVal = b.excluded ? -1 : b.execution_count; break;
          case 'lines': aVal = parseFloat(a.line_coverage) || 0; bVal = parseFloat(b.line_coverage) || 0; break;
          case 'branches': aVal = parseFloat(a.branch_coverage) || 0; bVal = parseFloat(b.branch_coverage) || 0; break;
          case 'conditions': aVal = parseFloat(a.condition_coverage) || 0; bVal = parseFloat(b.condition_coverage) || 0; break;
          default: aVal = a.name; bVal = b.name;
        }
        if (typeof aVal === 'string' && typeof bVal === 'string') {
          return ascending ? aVal.localeCompare(bVal) : bVal.localeCompare(aVal);
        }
        return ascending ? aVal - bVal : bVal - aVal;
      });
      lastStartIdx = -1; // force re-render
      viewport.style.height = (data.length * ROW_HEIGHT) + 'px';
      renderVisible();
    }

    // Intercept sort clicks on functions-header before initSorting runs
    var funcHeaders = document.querySelectorAll('.functions-header .sortable');
    funcHeaders.forEach(function(header) {
      header.addEventListener('click', function(e) {
        e.stopPropagation();
        var sortKey = this.dataset.sort;
        var isAscending = this.classList.contains('sorted-ascending');

        // Update header classes
        funcHeaders.forEach(function(h) {
          h.classList.remove('sorted-ascending', 'sorted-descending');
        });
        this.classList.add(isAscending ? 'sorted-descending' : 'sorted-ascending');

        sortData(sortKey, !isAscending);
      }, true); // capture phase to beat initSorting
    });

    // --- Restore saved state (scroll + highlight) ---
    function restoreSavedState() {
      var saved = sessionStorage.getItem('gcovr-functions-clicked');
      if (saved !== null) {
        sessionStorage.removeItem('gcovr-functions-clicked');
        highlightKey = saved;
      }
      var scroll = sessionStorage.getItem('gcovr-functions-scrollTop');
      if (scroll !== null) {
        sessionStorage.removeItem('gcovr-functions-scrollTop');
        container.scrollTop = parseInt(scroll, 10);
      }
      if (saved !== null || scroll !== null) {
        lastStartIdx = -1;
        renderVisible();
      }
    }

    // --- Initialize ---
    data.sort(function(a, b) { return a.name.localeCompare(b.name); });

    setupVirtualScroll();
    renderVisible();
    restoreSavedState();

    // Also restore on bfcache navigation (browser Back button)
    window.addEventListener('pageshow', function(e) {
      if (e.persisted) restoreSavedState();
    });

    // Mark functions page so initSorting can skip it
    container.dataset.virtualScroll = 'true';
  }



  // ===========================================
  // Sorting
  // ===========================================

  function initSorting() {
    var headerSets = [
      {
        selector: '.file-list-header .sortable, .functions-header .sortable',
        getContainer: function() {
          return document.getElementById('file-list') || document.querySelector('.functions-body');
        },
        defaultSort: { key: 'filename', ascending: true }
      },
      {
        selector: '.source-function-header .sortable',
        getContainer: function() {
          return document.querySelector('.source-functions-list');
        },
        defaultSort: null
      }
    ];

    headerSets.forEach(function(set) {
      var headers = document.querySelectorAll(set.selector);
      if (!headers.length) return;

      headers.forEach(function(header) {
        header.addEventListener('click', function() {
          var sortKey = this.dataset.sort;
          var isAscending = this.classList.contains('sorted-ascending');

          headers.forEach(function(h) {
            h.classList.remove('sorted-ascending', 'sorted-descending');
          });

          this.classList.add(isAscending ? 'sorted-descending' : 'sorted-ascending');

          sortList(set.getContainer(), sortKey, !isAscending);
        });
      });

      if (set.defaultSort) {
        sortList(set.getContainer(), set.defaultSort.key, set.defaultSort.ascending);
      }
    });
  }

  function sortList(container, key, ascending) {
    if (!container) return;
    // Virtual scroll handles its own sorting
    if (container.dataset.virtualScroll) return;

    var headerEl = container.querySelector('.source-function-header, .file-list-header, .functions-header');
    var rows = Array.from(container.children).filter(function(el) { return el !== headerEl; });

    rows.sort(function(a, b) {
      // Directories always come first
      var aIsDir = a.classList.contains('directory');
      var bIsDir = b.classList.contains('directory');
      if (aIsDir && !bIsDir) return -1;
      if (!aIsDir && bIsDir) return 1;

      var aVal = a.dataset[key] || a.querySelector('[data-sort]')?.dataset.sort || '';
      var bVal = b.dataset[key] || b.querySelector('[data-sort]')?.dataset.sort || '';
      if (key == 'filename' && localStorage.getItem('gcovr-view-mode') === 'nested') {
        aVal = aVal.split('/').pop();
        bVal = bVal.split('/').pop();
      }

      // Try to parse as numbers
      var aNum = parseFloat(aVal);
      var bNum = parseFloat(bVal);

      if (!isNaN(aNum) && !isNaN(bNum)) {
        return ascending ? aNum - bNum : bNum - aNum;
      }

      // String comparison
      return ascending ? aVal.localeCompare(bVal) : bVal.localeCompare(aVal);
    });

    rows.forEach(function(row) {
      container.appendChild(row);
    });
  }

  // ===========================================
  // Toggle Buttons (Coverage Lines)
  // ===========================================

  function initToggleButtons() {
    const buttons = document.querySelectorAll('.button_toggle_coveredLine, .button_toggle_uncoveredLine, .button_toggle_partialCoveredLine, .button_toggle_excludedLine');

    buttons.forEach(function(button) {
      var lineClass = button.value;
      if (!document.querySelector('.' + lineClass)) {
        button.disabled = true;
        button.classList.remove('show_' + lineClass);
        return;
      }
      button.addEventListener('click', function() {
        const lineClass = this.value;
        const showClass = 'show_' + lineClass;

        // Toggle the button state
        this.classList.toggle(showClass);

        // Toggle visibility of lines
        const lines = document.querySelectorAll('.' + lineClass);
        lines.forEach(function(line) {
          line.classList.toggle(showClass);
        });
        document.dispatchEvent(new CustomEvent('coverage-toggled'));
      });
    });

    // Also handle simpler toggle buttons
    const simpleToggles = document.querySelectorAll('.btn-toggle');
    simpleToggles.forEach(function(button) {
      button.addEventListener('click', function() {
        // Use data attribute to get line class (persists after toggle)
        const lineClass = this.dataset.lineClass;
        if (!lineClass) return;

        const showClass = 'show_' + lineClass;
        this.classList.toggle(showClass);
        const lines = document.querySelectorAll('.' + lineClass);
        lines.forEach(function(line) {
          line.classList.toggle(showClass);
        });
        document.dispatchEvent(new CustomEvent('coverage-toggled'));
      });
    });
  }

  // ===========================================
  // Coverage Navigation (prev/next uncovered)
  // ===========================================

  function initCoverageNav() {
    var prevBtn = document.getElementById('nav-prev');
    var nextBtn = document.getElementById('nav-next');
    var counter = document.getElementById('nav-counter');

    if (!prevBtn || !nextBtn || !counter) return;

    var gapLines = [];
    var currentIndex = -1;

    function collectGapLines() {
      var uncovered = document.querySelectorAll('tr.uncoveredLine.show_uncoveredLine');
      var partial = document.querySelectorAll('tr.partialCoveredLine.show_partialCoveredLine');
      var merged = [];
      var i;
      for (i = 0; i < uncovered.length; i++) merged.push(uncovered[i]);
      for (i = 0; i < partial.length; i++) merged.push(partial[i]);
      // Sort by DOM order
      merged.sort(function(a, b) {
        var pos = a.compareDocumentPosition(b);
        if (pos & Node.DOCUMENT_POSITION_FOLLOWING) return -1;
        if (pos & Node.DOCUMENT_POSITION_PRECEDING) return 1;
        return 0;
      });
      gapLines = merged;
      currentIndex = -1;
      updateCounter();
    }

    function updateCounter() {
      if (gapLines.length === 0) {
        counter.textContent = 'All lines covered';
        prevBtn.disabled = true;
        nextBtn.disabled = true;
      } else {
        var display = currentIndex >= 0 ? (currentIndex + 1) : 0;
        counter.textContent = display + ' / ' + gapLines.length;
        prevBtn.disabled = false;
        nextBtn.disabled = false;
      }
    }

    function navigateTo(index) {
      if (gapLines.length === 0) return;
      // Remove previous highlight
      var prev = document.querySelector('tr.source-line.nav-highlight');
      if (prev) prev.classList.remove('nav-highlight');

      currentIndex = index;
      var row = gapLines[currentIndex];
      row.scrollIntoView({ block: 'center', behavior: 'instant' });
      row.classList.add('nav-highlight');
      setTimeout(function() {
        row.classList.remove('nav-highlight');
      }, 1500);
      updateCounter();
    }

    function nextGap() {
      if (gapLines.length === 0) return;
      var next = currentIndex + 1;
      if (next >= gapLines.length) next = 0;
      navigateTo(next);
    }

    function prevGap() {
      if (gapLines.length === 0) return;
      var prev = currentIndex - 1;
      if (prev < 0) prev = gapLines.length - 1;
      navigateTo(prev);
    }

    prevBtn.addEventListener('click', prevGap);
    nextBtn.addEventListener('click', nextGap);

    document.addEventListener('keydown', function(e) {
      var tag = (e.target.tagName || '').toLowerCase();
      if (tag === 'input' || tag === 'textarea' || e.target.isContentEditable) return;
      if (e.key === 'n') nextGap();
      if (e.key === 'p') prevGap();
    });

    document.addEventListener('coverage-toggled', function() {
      collectGapLines();
    });

    collectGapLines();
  }

  // ===========================================
  // View Toggle (Nested / Flat)
  // ===========================================

  function initViewToggle() {
    var toggleContainer = document.getElementById('view-toggle');
    var fileList = document.getElementById('file-list');
    var appContainer = document.querySelector('.app-container');

    if (!toggleContainer) return;

    // Always show the toggle
    toggleContainer.style.display = '';

    var buttons = toggleContainer.querySelectorAll('.view-btn');
    var savedView = localStorage.getItem('gcovr-view-mode');

    function setActiveButton(view) {
      buttons.forEach(function(btn) {
        btn.classList.toggle('active', btn.dataset.view === view);
      });
    }

    // On non-directory pages (file/source views), still respect flat mode for sidebar
    if (!fileList) {
      if (appContainer && savedView === 'flat') {
        appContainer.classList.add('flat-mode');
        setActiveButton('flat');
      }

      // Allow toggling view mode from source pages
      buttons.forEach(function(btn) {
        btn.addEventListener('click', function() {
          var view = this.dataset.view;
          localStorage.setItem('gcovr-view-mode', view);
          setActiveButton(view);
          if (appContainer) {
            if (view === 'flat') {
              appContainer.classList.add('flat-mode');
            } else {
              appContainer.classList.remove('flat-mode');
              document.documentElement.classList.remove('early-flat-mode');
            }
          }
        });
      });
      return;
    }

    var originalNodes = null; // stash for restoring nested view

    function collectFlatFiles(nodes, parentPath) {
      var results = [];
      for (var i = 0; i < nodes.length; i++) {
        var node = nodes[i];
        var cleanedName = node.name;
        // Remove leading ./ or ../
        while (cleanedName.indexOf('./') === 0 || cleanedName.indexOf('../') === 0) {
          cleanedName = cleanedName.indexOf('./') === 0 ? cleanedName.substring(2) : cleanedName.substring(3);
        }
        var fullPath = parentPath ? (parentPath + '/' + cleanedName) : cleanedName;

        if (node.isDirectory && node.children && node.children.length > 0) {
          results = results.concat(collectFlatFiles(node.children, fullPath));
        } else if (!node.isDirectory) {
          var copy = {};
          for (var key in node) {
            if (node.hasOwnProperty(key)) copy[key] = node[key];
          }
          copy.fullPath = fullPath;
          results.push(copy);
        }
      }
      return results;
    }

    function buildFlatRow(file) {
      var row = document.createElement('div');
      row.className = 'file-row file';
      row.setAttribute('data-filename', file.fullPath);
      row.setAttribute('data-coverage', file.coverage || '0');
      row.setAttribute('data-lines', file.linesTotal || '');
      row.setAttribute('data-functions', file.functionsCoverage || '');
      row.setAttribute('data-branches', file.branchesCoverage || '');

      // Col name
      var colName = document.createElement('div');
      colName.className = 'col-name';

      var icon = document.createElement('span');
      icon.className = 'file-icon';
      icon.innerHTML = '<svg viewBox="0 0 16 16" width="16" height="16"><path fill="currentColor" d="M3.75 1.5a.25.25 0 00-.25.25v12.5c0 .138.112.25.25.25h9.5a.25.25 0 00.25-.25V6h-2.75A1.75 1.75 0 019 4.25V1.5H3.75zm6.75.062V4.25c0 .138.112.25.25.25h2.688a.252.252 0 00-.011-.013l-2.914-2.914a.272.272 0 00-.013-.011zM2 1.75C2 .784 2.784 0 3.75 0h6.586c.464 0 .909.184 1.237.513l2.914 2.914c.329.328.513.773.513 1.237v9.586A1.75 1.75 0 0113.25 16h-9.5A1.75 1.75 0 012 14.25V1.75z"></path></svg>';
      colName.appendChild(icon);

      if (file.link) {
        var a = document.createElement('a');
        a.href = file.link;
        a.textContent = file.fullPath;
        a.title = file.fullPath;
        colName.appendChild(a);
      } else {
        var span = document.createElement('span');
        span.className = 'no-link';
        span.textContent = file.fullPath;
        span.title = file.fullPath;
        colName.appendChild(span);
      }
      row.appendChild(colName);

      // Col coverage
      var colCov = document.createElement('div');
      colCov.className = 'col-coverage';

      var barContainer = document.createElement('div');
      barContainer.className = 'coverage-bar-container';
      var bar = document.createElement('div');
      var linesCov = file.linesCoverage || '';
      var linesClass = file.linesClass || file.coverageClass || '';
      bar.className = 'coverage-bar ' + linesClass;
      bar.style.width = (linesCov && linesCov !== '-') ? linesCov + '%' : '0%';
      barContainer.appendChild(bar);
      colCov.appendChild(barContainer);

      var pct = document.createElement('span');
      pct.className = 'coverage-percent ' + linesClass;
      pct.textContent = (linesCov && linesCov !== '-') ? linesCov + '%' : '-';
      colCov.appendChild(pct);
      row.appendChild(colCov);

      // Col lines
      var colLines = document.createElement('div');
      colLines.className = 'col-lines';
      var execSpan = document.createElement('span');
      execSpan.className = 'stat-value';
      execSpan.textContent = file.linesExec || '';
      colLines.appendChild(execSpan);
      var sep = document.createElement('span');
      sep.className = 'stat-separator';
      sep.textContent = '/';
      colLines.appendChild(sep);
      var totalSpan = document.createElement('span');
      totalSpan.className = 'stat-total';
      totalSpan.textContent = file.linesTotal || '';
      colLines.appendChild(totalSpan);
      row.appendChild(colLines);

      // Col functions (check if container has the column)
      var container = fileList.closest('.file-list-container');
      var hasFunctions = !container || !container.classList.contains('no-functions');
      var hasBranches = !container || !container.classList.contains('no-branches');
      var hasConditions = !container || !container.classList.contains('no-conditions');
      var hasDecision = !container || !container.classList.contains('no-decisions');
      var hasCalls = !container || !container.classList.contains('no-calls');

      if (hasFunctions) {
        var colFunc = document.createElement('div');
        colFunc.className = 'col-functions';
        var funcVal = document.createElement('span');
        var funcCov = file.functionsCoverage || '';
        var funcClass = file.functionsClass || '';
        funcVal.className = 'stat-value ' + funcClass;
        funcVal.textContent = (funcCov && funcCov !== '-') ? funcCov + '%' : '-';
        colFunc.appendChild(funcVal);
        row.appendChild(colFunc);
      }

      if (hasBranches) {
        var colBr = document.createElement('div');
        colBr.className = 'col-branches';
        var brVal = document.createElement('span');
        var brCov = file.branchesCoverage || '';
        var brClass = file.branchesClass || '';
        brVal.className = 'stat-value ' + brClass;
        brVal.textContent = (brCov && brCov !== '-') ? brCov + '%' : '-';
        colBr.appendChild(brVal);
        row.appendChild(colBr);
      }

      if (hasConditions) {
        var colCond = document.createElement('div');
        colCond.className = 'col-conditions';
        var condVal = document.createElement('span');
        var condCov = file.conditionsCoverage || '';
        var condClass = file.conditionsClass || '';
        condVal.className = 'stat-value ' + condClass;
        condVal.textContent = (condCov && condCov !== '-') ? condCov + '%' : '-';
        colCond.appendChild(condVal);
        row.appendChild(colCond);
      }

      if (hasDecision) {
        var colDec = document.createElement('div');
        colDec.className = 'col-decision';
        var decVal = document.createElement('span');
        var decCov = file.decisionCoverage || '';
        var decClass = file.decisionClass || '';
        decVal.className = 'stat-value ' + decClass;
        decVal.textContent = (decCov && decCov !== '-') ? decCov + '%' : '-';
        colDec.appendChild(decVal);
        row.appendChild(colDec);
      }

      if (hasCalls) {
        var colCalls = document.createElement('div');
        colCalls.className = 'col-calls';
        var callsVal = document.createElement('span');
        var callsCov = file.callsCoverage || '';
        var callsClass = file.callsClass || '';
        callsVal.className = 'stat-value ' + callsClass;
        callsVal.textContent = (callsCov && callsCov !== '-') ? callsCov + '%' : '-';
        colCalls.appendChild(callsVal);
        row.appendChild(colCalls);
      }


      return row;
    }

    function switchToFlat() {
      if (!window.GCOVR_TREE_DATA) return;

      // Stash original DOM nodes
      if (originalNodes === null) {
        originalNodes = document.createDocumentFragment();
        while (fileList.firstChild) {
          originalNodes.appendChild(fileList.firstChild);
        }
      }

      var flatFiles = collectFlatFiles(window.GCOVR_TREE_DATA, '');

      // Sort by coverage ascending (matching default)
      flatFiles.sort(function(a, b) {
        var aVal = parseFloat(a.coverage) || 0;
        var bVal = parseFloat(b.coverage) || 0;
        return aVal - bVal;
      });

      while (fileList.firstChild) {
        fileList.removeChild(fileList.firstChild);
      }
      for (var i = 0; i < flatFiles.length; i++) {
        fileList.appendChild(buildFlatRow(flatFiles[i]));
      }

      if (appContainer) appContainer.classList.add('flat-mode');
      setActiveButton('flat');
      localStorage.setItem('gcovr-view-mode', 'flat');
    }

    function switchToNested() {
      if (originalNodes !== null) {
        while (fileList.firstChild) {
          fileList.removeChild(fileList.firstChild);
        }
        fileList.appendChild(originalNodes);
        originalNodes = null;
      }
      if (appContainer) appContainer.classList.remove('flat-mode');
      document.documentElement.classList.remove('early-flat-mode');
      setActiveButton('nested');
      localStorage.setItem('gcovr-view-mode', 'nested');

      // Re-run sorting to maintain state
      sortList(document.getElementById('file-list') || document.querySelector('.functions-body'), 'filename', true);
    }

    buttons.forEach(function(btn) {
      btn.addEventListener('click', function() {
        var view = this.dataset.view;
        if (view === 'flat') {
          switchToFlat();
        } else {
          switchToNested();
        }
      });
    });

    // Apply saved preference on load
    if (savedView === 'flat') {
      // Defer to ensure tree data is loaded
      setTimeout(function() {
        switchToFlat();
      }, 0);
    }
  }

  // ===========================================
  // Settings Dropdown (mobile gear icon)
  // ===========================================

  function initSettingsDropdown() {
    var btn = document.getElementById('settings-btn');
    var dropdown = document.getElementById('settings-dropdown');
    var header = document.querySelector('.main-header');
    if (!btn || !dropdown || !header) return;

    var viewToggle = document.getElementById('view-toggle');
    var themeToggle = document.getElementById('theme-toggle');
    var isMobile = false;

    // Reference node: settings-btn, so we can insert before it when moving back
    function moveToDropdown() {
      if (viewToggle && viewToggle.parentNode !== dropdown) {
        dropdown.appendChild(viewToggle);
      }
      if (themeToggle && themeToggle.parentNode !== dropdown) {
        dropdown.appendChild(themeToggle);
      }
    }

    function moveToHeader() {
      // Insert before settings-btn so they appear in original order
      if (viewToggle && viewToggle.parentNode !== header) {
        header.insertBefore(viewToggle, btn);
      }
      if (themeToggle && themeToggle.parentNode !== header) {
        header.insertBefore(themeToggle, btn);
      }
    }

    function checkBreakpoint() {
      var nowMobile = window.innerWidth <= 1024;
      if (nowMobile === isMobile) return;
      isMobile = nowMobile;
      if (isMobile) {
        moveToDropdown();
      } else {
        dropdown.classList.remove('open');
        moveToHeader();
      }
    }

    // Toggle dropdown on button click
    btn.addEventListener('click', function(e) {
      e.stopPropagation();
      dropdown.classList.toggle('open');
    });

    // Close on outside click
    document.addEventListener('click', function(e) {
      if (!dropdown.contains(e.target) && e.target !== btn) {
        dropdown.classList.remove('open');
      }
    });

    // Close on Escape
    document.addEventListener('keydown', function(e) {
      if (e.key === 'Escape') {
        dropdown.classList.remove('open');
      }
    });

    // Respond to resize
    window.addEventListener('resize', checkBreakpoint);

    // Initial check
    checkBreakpoint();
  }

  // ===========================================
  // Popup Resize (only when overflowing)
  // ===========================================

  function initPopupResize() {
    var details = document.querySelectorAll('.branch-details, .condition-details, .decision-details, .call-details');
    if (details.length === 0) return;

    details.forEach(function(det) {
      det.addEventListener('toggle', function() {
        if (!det.open) return;
        var popup = det.querySelector('.branch-popup, .condition-popup, .decision-popup, .call-popup');
        if (!popup) return;
        // Check after render if content overflows
        requestAnimationFrame(function() {
          if (popup.scrollHeight > popup.clientHeight) {
            popup.classList.add('is-overflowing');
          } else {
            popup.classList.remove('is-overflowing');
          }
        });
      });
    });
  }

  // ===========================================
  // Nav Override (prev/next follows tree order)
  // ===========================================

  function initNavOverride() {
    if (!window.GCOVR_TREE_DATA) return;

    var navPrev = document.querySelectorAll('.nav-prev');
    var navNext = document.querySelectorAll('.nav-next');
    if (navPrev.length === 0 && navNext.length === 0) return;

    // DFS-flatten tree to collect file links in sidebar order
    function collectLinks(nodes) {
      var links = [];
      for (var i = 0; i < nodes.length; i++) {
        var node = nodes[i];
        if (node.isDirectory && node.children && node.children.length > 0) {
          links = links.concat(collectLinks(node.children));
        } else if (!node.isDirectory && node.link) {
          links.push(node.link);
        }
      }
      return links;
    }

    var fileLinks = collectLinks(window.GCOVR_TREE_DATA);
    if (fileLinks.length === 0) return;

    var currentPage = window.location.pathname.split('/').pop() || 'index.html';
    var idx = fileLinks.indexOf(currentPage);
    if (idx === -1) return;

    var prev = idx > 0 ? fileLinks[idx - 1] : null;
    var next = idx < fileLinks.length - 1 ? fileLinks[idx + 1] : null;

    function updateNavLinks(els, href) {
      for (var i = 0; i < els.length; i++) {
        var el = els[i];
        if (href) {
          // Enable: ensure it's an <a> with the correct href
          if (el.tagName === 'A') {
            el.setAttribute('href', href);
          } else {
            // Replace disabled <span> with an <a>
            var a = document.createElement('a');
            a.className = el.className.replace(/\bdisabled\b/, '').trim();
            a.href = href;
            a.title = el.title;
            while (el.firstChild) a.appendChild(el.firstChild);
            el.parentNode.replaceChild(a, el);
          }
        } else {
          // Disable: ensure it's a <span> with disabled class
          if (el.tagName === 'A') {
            var span = document.createElement('span');
            span.className = el.className + ' disabled';
            span.title = el.title;
            while (el.firstChild) span.appendChild(el.firstChild);
            el.parentNode.replaceChild(span, el);
          } else {
            el.classList.add('disabled');
          }
        }
      }
    }

    updateNavLinks(navPrev, prev);
    updateNavLinks(navNext, next);
  }

  // ===========================================
  // TLA Navigation (HIT/MIS/PAR links)
  // ===========================================

  function initTlaNavigation() {
    var rows = document.querySelectorAll('.source-line');
    if (rows.length === 0) return;

    // Classify each row by coverage type
    var COV_CLASSES = ['coveredLine', 'uncoveredLine', 'partialCoveredLine'];
    var LABELS = { coveredLine: 'HIT', uncoveredLine: 'MIS', partialCoveredLine: 'PAR' };
    var CSS_CLASSES = { coveredLine: 'tla-hit', uncoveredLine: 'tla-mis', partialCoveredLine: 'tla-par' };

    // Build list of groups: contiguous runs of the same coverage class
    var groups = []; // { type, firstRow }
    var prevType = null;

    for (var i = 0; i < rows.length; i++) {
      var row = rows[i];
      var type = null;
      for (var j = 0; j < COV_CLASSES.length; j++) {
        if (row.classList.contains(COV_CLASSES[j])) {
          type = COV_CLASSES[j];
          break;
        }
      }
      if (type === null) {
        prevType = null;
        continue;
      }
      if (type !== prevType) {
        groups.push({ type: type, firstRow: row });
        prevType = type;
      }
    }

    if (groups.length === 0) return;

    // Determine the anchor prefix used in this page
    var sampleAnchor = rows[0].querySelector('.col-lineno a');
    var anchorPrefix = '';
    if (sampleAnchor) {
      var id = sampleAnchor.id;
      var idx = id.indexOf('l');
      if (idx > 0) {
        anchorPrefix = id.substring(0, idx);
      }
    }

    // For each group, find the line number from its first row
    function getLineNo(row) {
      var a = row.querySelector('.col-lineno a');
      return a ? a.textContent.trim() : '';
    }

    // Build per-type lists for wrap-around linking
    var byType = {};
    for (var i = 0; i < groups.length; i++) {
      var g = groups[i];
      if (!byType[g.type]) byType[g.type] = [];
      byType[g.type].push(i);
    }

    // For each group, compute next group index of same type
    var nextGroupIdx = new Array(groups.length);
    for (var type in byType) {
      var indices = byType[type];
      for (var k = 0; k < indices.length; k++) {
        var nextK = (k + 1) % indices.length;
        nextGroupIdx[indices[k]] = indices[nextK];
      }
    }

    // Inject TLA links
    for (var i = 0; i < groups.length; i++) {
      var g = groups[i];
      var cell = g.firstRow.querySelector('.col-tla');
      if (!cell) continue;

      var targetGroup = groups[nextGroupIdx[i]];
      var targetLineNo = getLineNo(targetGroup.firstRow);

      var targetId = anchorPrefix + 'l' + targetLineNo;

      var a = document.createElement('a');
      a.className = 'tla-link ' + CSS_CLASSES[g.type];
      a.textContent = LABELS[g.type];
      a.href = '#' + targetId;
      a.addEventListener('click', function(e) {
        var target = document.getElementById(this.getAttribute('href').substring(1));
        if (target) {
          e.preventDefault();
          // Scroll within the source-table-container
          var scrollBox = document.querySelector('.source-table-container');
          var row = target.closest('tr');
          if (scrollBox && row) {
            var thead = scrollBox.querySelector('thead');
            var theadHeight = thead ? thead.offsetHeight : 0;
            scrollBox.scrollTo({ top: row.offsetTop - theadHeight - 8, behavior: 'instant' });
          }
          history.replaceState(null, '', this.getAttribute('href'));
          // Highlight the target row (clear any previous highlight first)
          var prev = document.querySelector('.highlight-target');
          if (prev) prev.classList.remove('highlight-target');
          if (row) row.classList.add('highlight-target');
        }
      });
      cell.appendChild(a);
    }
  }

  // ===========================================
  // Line number click highlight
  // ===========================================

  function initLineHighlight() {
    var clickedFnItem = null;

    function highlightFromHash(scroll) {
      var prev = document.querySelector('.highlight-target');
      if (prev) prev.classList.remove('highlight-target');
      var prevFn = document.querySelector('.source-function-item.selected');
      if (prevFn) prevFn.classList.remove('selected');
      var id = window.location.hash.slice(1);
      if (!id) return;
      var el = document.getElementById(id);
      if (!el) return;
      var fnItem = clickedFnItem || document.querySelector('.source-function-item[href="#' + id + '"]');
      clickedFnItem = null;
      if (fnItem) fnItem.classList.add('selected');
      var row = el.closest('tr');
      if (row) {
        row.classList.add('highlight-target');
        if (scroll) {
          var scrollBox = document.querySelector('.source-table-container');
          if (scrollBox) {
            var thead = scrollBox.querySelector('thead');
            var theadHeight = thead ? thead.offsetHeight : 0;
            scrollBox.scrollTo({ top: row.offsetTop - theadHeight - 8, behavior: 'instant' });
          } else {
            row.scrollIntoView({ block: 'center' });
          }
        }
      }
    }

    // Handle clicks on function list items directly
    var fnList = document.querySelector('.source-functions-list');
    if (fnList) {
      fnList.addEventListener('click', function(e) {
        var item = e.target.closest('.source-function-item');
        if (!item) return;
        e.preventDefault();
        clickedFnItem = item;
        var href = item.getAttribute('href');
        if (href) history.replaceState(null, '', href);
        highlightFromHash(true);
      });
    }

    // Event delegation: single listener on the table container
    var container = document.querySelector('.source-table-container');
    if (container) {
      container.addEventListener('click', function(e) {
        var anchor = e.target.closest('.col-lineno a');
        if (!anchor) return;
        e.preventDefault();
        if (anchor.id) history.replaceState(null, '', '#' + anchor.id);
        highlightFromHash(false);
      });
    }

    // Highlight + scroll on initial load and back/forward navigation
    highlightFromHash(true);
    window.addEventListener('hashchange', function() { highlightFromHash(true); });
  }

  // ===========================================
  // Column Visibility Toggles
  // ===========================================

  function initColumnToggles() {
    var buttons = document.querySelectorAll('.col-toggle');
    if (buttons.length === 0) return;

    var table = document.querySelector('.source-table');
    if (!table) return;

    // Restore saved state
    var hidden = [];
    try {
      var saved = localStorage.getItem('gcovr-hidden-columns');
      if (saved) {
        hidden = JSON.parse(saved);
      } else {
        hidden = ['tla'];
      }
    } catch (e) {}

    // Apply saved hidden columns
    var fnList = document.querySelector('.source-functions-list');
    for (var i = 0; i < hidden.length; i++) {
      table.classList.add('hide-col-' + hidden[i]);
      if (fnList) {
        fnList.classList.add('hide-col-' + hidden[i]);
      }
    }

    // Update button appearance to match state
    buttons.forEach(function(btn) {
      var col = btn.getAttribute('data-col');
      if (hidden.indexOf(col) >= 0) {
        btn.classList.remove('show-col');
      }
    });

    // Handle clicks
    buttons.forEach(function(btn) {
      btn.addEventListener('click', function() {
        var col = this.getAttribute('data-col');
        var hideClass = 'hide-col-' + col;
        var isHidden = table.classList.toggle(hideClass);
        this.classList.toggle('show-col', !isHidden);

        // Sync with function list sidebar
        var fnList = document.querySelector('.source-functions-list');
        if (fnList) {
          fnList.classList.toggle(hideClass, isHidden);
        }

        // Save state
        var current = [];
        var allBtns = document.querySelectorAll('.col-toggle');
        allBtns.forEach(function(b) {
          if (!b.classList.contains('show-col')) {
            current.push(b.getAttribute('data-col'));
          }
        });
        localStorage.setItem('gcovr-hidden-columns', JSON.stringify(current));
      });
    });
  }

  // ===========================================
  // File nav keyboard shortcuts ([ and ])
  // ===========================================

  function initFileNavKeys() {
    var prevLink = document.querySelector('.source-nav-links .nav-prev') || document.querySelector('.nav-links .nav-prev');
    var nextLink = document.querySelector('.source-nav-links .nav-next') || document.querySelector('.nav-links .nav-next');
    if (!prevLink && !nextLink) return;

    document.addEventListener('keydown', function(e) {
      var tag = (e.target.tagName || '').toLowerCase();
      if (tag === 'input' || tag === 'textarea' || e.target.isContentEditable) return;
      // Re-query to pick up any DOM replacements by initNavOverride
      var prev = document.querySelector('.source-nav-links a.nav-prev') || document.querySelector('.nav-links a.nav-prev');
      var next = document.querySelector('.source-nav-links a.nav-next') || document.querySelector('.nav-links a.nav-next');
      if (e.key === '[' && prev) {
        window.location.href = prev.href;
      }
      if (e.key === ']' && next) {
        window.location.href = next.href;
      }
    });
  }

  // ===========================================
  // Enrich file nav tooltips with actual filenames
  // ===========================================

  function initFileNavTooltips() {
    if (!window.GCOVR_TREE_DATA) return;
    var links = document.querySelectorAll('.source-nav-links .nav-prev, .source-nav-links .nav-next, .nav-links .nav-prev, .nav-links .nav-next');
    for (var i = 0; i < links.length; i++) {
      var anchor = links[i];
      var href = anchor.getAttribute('href');
      if (!href || href === '#') continue;
      var filename = href.replace(/^.*\//, '').replace(/#.*$/, '');
      var node = findNodeInTree(window.GCOVR_TREE_DATA, filename);
      if (node && node.name) {
        var direction = anchor.classList.contains('nav-prev') ? 'Previous' : 'Next';
        anchor.title = direction + ': ' + node.name;
      }
    }
  }

  function findNodeInTree(nodes, targetLink) {
    for (var i = 0; i < nodes.length; i++) {
      var node = nodes[i];
      if (node.link === targetLink) return node;
      if (node.children) {
        var found = findNodeInTree(node.children, targetLink);
        if (found) return found;
      }
    }
    return null;
  }

  // ===========================================
  // Prefetch pages on hover for instant nav
  // ===========================================

  function initPrefetch() {
    // Skip for file:// protocol (fetch won't work)
    if (location.protocol === 'file:') return;

    var prefetched = {};

    document.addEventListener('mouseover', function(e) {
      var link = e.target.closest('a[href]');
      if (!link) return;

      var href = link.getAttribute('href');
      // Only prefetch local HTML pages
      if (!href || href.charAt(0) === '#' || href.indexOf('://') !== -1) return;
      if (prefetched[href]) return;

      prefetched[href] = true;
      var prefetchLink = document.createElement('link');
      prefetchLink.rel = 'prefetch';
      prefetchLink.href = href;
      document.head.appendChild(prefetchLink);
    });
  }

  function initFunctionListPersistence() {
    var details = document.querySelector('details.source-functions');
    if (!details) return;

    var key = 'gcovr-fn-list-open';
    if (sessionStorage.getItem(key) === 'true') {
      details.setAttribute('open', '');
    }

    details.addEventListener('toggle', function() {
      sessionStorage.setItem(key, details.open ? 'true' : 'false');
    });
  }

})();

window.GCOVR_TREE_DATA = [{"name": "include", "coverage": "91.1", "coverageClass": "coverage-high", "linesTotal": "7607", "linesExec": "6931", "linesCoverage": "91.1", "linesClass": "coverage-high", "functionsCoverage": "96.6", "functionsClass": "coverage-high", "branchesCoverage": "44.9", "branchesClass": "coverage-low", "isDirectory": true, "link": "index.include.d436eb0fd9de10b54a828ce6435f7e81.html", "children": [{"name": "boost", "coverage": "", "coverageClass": "coverage-unknown", "linesTotal": "", "linesExec": "", "linesCoverage": "", "linesClass": "", "functionsCoverage": "", "functionsClass": "", "branchesCoverage": "", "branchesClass": "", "isDirectory": true, "link": null, "children": [{"name": "corosio", "coverage": "", "coverageClass": "coverage-unknown", "linesTotal": "", "linesExec": "", "linesCoverage": "", "linesClass": "", "functionsCoverage": "", "functionsClass": "", "branchesCoverage": "", "branchesClass": "", "isDirectory": true, "link": null, "children": [{"name": "detail", "coverage": "91.7", "coverageClass": "coverage-high", "linesTotal": "783", "linesExec": "718", "linesCoverage": "91.7", "linesClass": "coverage-high", "functionsCoverage": "97.2", "functionsClass": "coverage-high", "branchesCoverage": "48.4", "branchesClass": "coverage-low", "isDirectory": true, "link": "index.detail.28b3a5c34bfd9f2f42154012b6bbd57e.html", "children": [{"name": "buffer_param.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "26", "linesExec": "26", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "86.0", "branchesClass": "coverage-medium", "isDirectory": false, "link": "index.buffer_param.hpp.9fa32aed3e21e3bfecc23c12e86aae8f.html", "children": []}, {"name": "cancel_at_awaitable.hpp", "coverage": "94.5", "coverageClass": "coverage-high", "linesTotal": "55", "linesExec": "52", "linesCoverage": "94.5", "linesClass": "coverage-high", "functionsCoverage": "90.6", "functionsClass": "coverage-high", "branchesCoverage": "50.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.cancel_at_awaitable.hpp.8b1c6bcd0fae2281baa12e2c56d991e5.html", "children": []}, {"name": "conditionally_enabled_event.hpp", "coverage": "65.2", "coverageClass": "coverage-low", "linesTotal": "23", "linesExec": "15", "linesCoverage": "65.2", "linesClass": "coverage-low", "functionsCoverage": "71.4", "functionsClass": "coverage-low", "branchesCoverage": "37.5", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.conditionally_enabled_event.hpp.55f4d7412fc066f1d6f24c8bb8092c9f.html", "children": []}, {"name": "conditionally_enabled_mutex.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "28", "linesExec": "28", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "78.6", "branchesClass": "coverage-medium", "isDirectory": false, "link": "index.conditionally_enabled_mutex.hpp.b776cf8fd73bb99d1100558f3b9c1793.html", "children": []}, {"name": "continuation_op.hpp", "coverage": "67.9", "coverageClass": "coverage-low", "linesTotal": "28", "linesExec": "19", "linesCoverage": "67.9", "linesClass": "coverage-low", "functionsCoverage": "85.7", "functionsClass": "coverage-medium", "branchesCoverage": "37.5", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.continuation_op.hpp.45fcf1a8b8ed7016669c36477cce22b9.html", "children": []}, {"name": "dispatch_coro.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "5", "linesExec": "5", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "100.0", "branchesClass": "coverage-high", "isDirectory": false, "link": "index.dispatch_coro.hpp.4c3fb81766654c945218f91ce0093c44.html", "children": []}, {"name": "file_service.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "2", "linesExec": "2", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.file_service.hpp.47ae5e985fb8c0a30fff3728a684b62b.html", "children": []}, {"name": "intrusive.hpp", "coverage": "96.1", "coverageClass": "coverage-high", "linesTotal": "76", "linesExec": "73", "linesCoverage": "96.1", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "37.6", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.intrusive.hpp.0d94121c15ba2d09ffd0f0ee4dca2d9f.html", "children": []}, {"name": "local_datagram_service.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "2", "linesExec": "2", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.local_datagram_service.hpp.9772169ce66dabbdec0fcf6455248259.html", "children": []}, {"name": "local_stream_acceptor_service.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "2", "linesExec": "2", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.local_stream_acceptor_service.hpp.812c04884a1f02717400b788ec43de21.html", "children": []}, {"name": "local_stream_service.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "2", "linesExec": "2", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.local_stream_service.hpp.16f38ba4fbf00536f0f049352733de0f.html", "children": []}, {"name": "op_base.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "25", "linesExec": "25", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "47.1", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.op_base.hpp.f042724d40333548952498755131c326.html", "children": []}, {"name": "random_access_file_service.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "2", "linesExec": "2", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.random_access_file_service.hpp.f2ff4f1a74012ae66ab4d71eed596489.html", "children": []}, {"name": "scheduler.hpp", "coverage": "33.3", "coverageClass": "coverage-low", "linesTotal": "3", "linesExec": "1", "linesCoverage": "33.3", "linesClass": "coverage-low", "functionsCoverage": "50.0", "functionsClass": "coverage-low", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.scheduler.hpp.b9966f659c05883b76a3929d20ab6946.html", "children": []}, {"name": "scheduler_op.hpp", "coverage": "44.4", "coverageClass": "coverage-low", "linesTotal": "9", "linesExec": "4", "linesCoverage": "44.4", "linesClass": "coverage-low", "functionsCoverage": "66.7", "functionsClass": "coverage-low", "branchesCoverage": "0.0", "branchesClass": "coverage-none", "isDirectory": false, "link": "index.scheduler_op.hpp.c938ee577fc126a3da9a3dd52e9e2976.html", "children": []}, {"name": "tcp_acceptor_service.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "2", "linesExec": "2", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.tcp_acceptor_service.hpp.235e6c32eaf3e1e7937b3f645e5d1af2.html", "children": []}, {"name": "tcp_service.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "2", "linesExec": "2", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.tcp_service.hpp.500dca11988cf66567c337846fc167c7.html", "children": []}, {"name": "thread_local_ptr.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "5", "linesExec": "5", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.thread_local_ptr.hpp.08ae8718bd5240c6de692c2546729658.html", "children": []}, {"name": "thread_pool.hpp", "coverage": "92.6", "coverageClass": "coverage-high", "linesTotal": "54", "linesExec": "50", "linesCoverage": "92.6", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "65.6", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.thread_pool.hpp.beedc9dd13eb4a6a3f689694b0af3128.html", "children": []}, {"name": "timeout_coro.hpp", "coverage": "97.1", "coverageClass": "coverage-high", "linesTotal": "34", "linesExec": "33", "linesCoverage": "97.1", "linesClass": "coverage-high", "functionsCoverage": "96.3", "functionsClass": "coverage-high", "branchesCoverage": "52.4", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.timeout_coro.hpp.b05637c1db0b9b0b780c21dfc21ea45c.html", "children": []}, {"name": "timer_service.hpp", "coverage": "92.4", "coverageClass": "coverage-high", "linesTotal": "396", "linesExec": "366", "linesCoverage": "92.4", "linesClass": "coverage-high", "functionsCoverage": "97.8", "functionsClass": "coverage-high", "branchesCoverage": "68.9", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.timer_service.hpp.af4aa31cf5e98ef232ef52d5fc328571.html", "children": []}, {"name": "udp_service.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "2", "linesExec": "2", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.udp_service.hpp.63440a7afd185263fb3ecbdc537ed56d.html", "children": []}]}, {"name": "io", "coverage": "93.0", "coverageClass": "coverage-high", "linesTotal": "158", "linesExec": "147", "linesCoverage": "93.0", "linesClass": "coverage-high", "functionsCoverage": "98.4", "functionsClass": "coverage-high", "branchesCoverage": "57.3", "branchesClass": "coverage-low", "isDirectory": true, "link": "index.io.29e11ccd645cee93dd7043af37bfcf15.html", "children": [{"name": "io_object.hpp", "coverage": "92.5", "coverageClass": "coverage-high", "linesTotal": "67", "linesExec": "62", "linesCoverage": "92.5", "linesClass": "coverage-high", "functionsCoverage": "95.7", "functionsClass": "coverage-high", "branchesCoverage": "56.7", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.io_object.hpp.c58fc563108a3d642a733957d9d108e2.html", "children": []}, {"name": "io_read_stream.hpp", "coverage": "88.9", "coverageClass": "coverage-medium", "linesTotal": "9", "linesExec": "8", "linesCoverage": "88.9", "linesClass": "coverage-medium", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "50.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.io_read_stream.hpp.208110520eae57000c27624fb80fcb02.html", "children": []}, {"name": "io_signal_set.hpp", "coverage": "96.0", "coverageClass": "coverage-high", "linesTotal": "25", "linesExec": "24", "linesCoverage": "96.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "60.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.io_signal_set.hpp.c7acb622dca278d47fd929b4ac681938.html", "children": []}, {"name": "io_stream.hpp", "coverage": "77.8", "coverageClass": "coverage-medium", "linesTotal": "9", "linesExec": "7", "linesCoverage": "77.8", "linesClass": "coverage-medium", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "50.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.io_stream.hpp.84de4cdd9b081d259c7a0ace80a4c19f.html", "children": []}, {"name": "io_timer.hpp", "coverage": "97.4", "coverageClass": "coverage-high", "linesTotal": "39", "linesExec": "38", "linesCoverage": "97.4", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "66.7", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.io_timer.hpp.6ff63a9ff56b66f3c4813d1ad4338ff4.html", "children": []}, {"name": "io_write_stream.hpp", "coverage": "88.9", "coverageClass": "coverage-medium", "linesTotal": "9", "linesExec": "8", "linesCoverage": "88.9", "linesClass": "coverage-medium", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "50.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.io_write_stream.hpp.08e88398e19315293bc38438092e853c.html", "children": []}]}, {"name": "native", "coverage": "90.2", "coverageClass": "coverage-high", "linesTotal": "5414", "linesExec": "4884", "linesCoverage": "90.2", "linesClass": "coverage-high", "functionsCoverage": "96.3", "functionsClass": "coverage-high", "branchesCoverage": "42.9", "branchesClass": "coverage-low", "isDirectory": true, "link": "index.native.e74b7429d096469e4f401c37e756bd44.html", "children": [{"name": "detail", "coverage": "89.8", "coverageClass": "coverage-medium", "linesTotal": "4350", "linesExec": "3908", "linesCoverage": "89.8", "linesClass": "coverage-medium", "functionsCoverage": "95.1", "functionsClass": "coverage-high", "branchesCoverage": "42.6", "branchesClass": "coverage-low", "isDirectory": true, "link": "index.detail.e9addf03bc19b769521ca765dd3e1168.html", "children": [{"name": "kqueue", "coverage": "87.8", "coverageClass": "coverage-medium", "linesTotal": "286", "linesExec": "251", "linesCoverage": "87.8", "linesClass": "coverage-medium", "functionsCoverage": "98.2", "functionsClass": "coverage-high", "branchesCoverage": "49.5", "branchesClass": "coverage-low", "isDirectory": true, "link": "index.kqueue.91be3352d3a408f96ec2b635dcddefdf.html", "children": [{"name": "kqueue_scheduler.hpp", "coverage": "90.1", "coverageClass": "coverage-high", "linesTotal": "141", "linesExec": "127", "linesCoverage": "90.1", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "54.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.kqueue_scheduler.hpp.14d2be9ca492878735f5cc0be5f52ad1.html", "children": []}, {"name": "kqueue_traits.hpp", "coverage": "82.9", "coverageClass": "coverage-medium", "linesTotal": "111", "linesExec": "92", "linesCoverage": "82.9", "linesClass": "coverage-medium", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "45.5", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.kqueue_traits.hpp.36db6468bf9bd02cd343a258c4f0fc32.html", "children": []}, {"name": "kqueue_types.hpp", "coverage": "94.1", "coverageClass": "coverage-high", "linesTotal": "34", "linesExec": "32", "linesCoverage": "94.1", "linesClass": "coverage-high", "functionsCoverage": "96.6", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.kqueue_types.hpp.146700c1853ff5933b12281e7f3c25f9.html", "children": []}]}, {"name": "posix", "coverage": "91.4", "coverageClass": "coverage-high", "linesTotal": "1260", "linesExec": "1152", "linesCoverage": "91.4", "linesClass": "coverage-high", "functionsCoverage": "95.9", "functionsClass": "coverage-high", "branchesCoverage": "68.3", "branchesClass": "coverage-low", "isDirectory": true, "link": "index.posix.6b7f187f84104dfe0e9604106bbb89c5.html", "children": [{"name": "posix_random_access_file.hpp", "coverage": "86.6", "coverageClass": "coverage-medium", "linesTotal": "119", "linesExec": "103", "linesCoverage": "86.6", "linesClass": "coverage-medium", "functionsCoverage": "89.5", "functionsClass": "coverage-medium", "branchesCoverage": "75.0", "branchesClass": "coverage-medium", "isDirectory": false, "link": "index.posix_random_access_file.hpp.6dd7590e2ae73356ae7db69b81fd93c0.html", "children": []}, {"name": "posix_random_access_file_service.hpp", "coverage": "89.3", "coverageClass": "coverage-medium", "linesTotal": "159", "linesExec": "142", "linesCoverage": "89.3", "linesClass": "coverage-medium", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "54.4", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.posix_random_access_file_service.hpp.e7dab3a88845007b830e87db6392f9ff.html", "children": []}, {"name": "posix_resolver.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "22", "linesExec": "22", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.posix_resolver.hpp.2e7880c1baa54f5ff3b936799052f92f.html", "children": []}, {"name": "posix_resolver_service.hpp", "coverage": "94.4", "coverageClass": "coverage-high", "linesTotal": "301", "linesExec": "284", "linesCoverage": "94.4", "linesClass": "coverage-high", "functionsCoverage": "93.5", "functionsClass": "coverage-high", "branchesCoverage": "77.3", "branchesClass": "coverage-medium", "isDirectory": false, "link": "index.posix_resolver_service.hpp.8aad9e0a1a0943e0df962e3c49e753ee.html", "children": []}, {"name": "posix_signal.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "13", "linesExec": "13", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.posix_signal.hpp.d8f21510f47b5178e79db8c4ed57eb39.html", "children": []}, {"name": "posix_signal_service.hpp", "coverage": "92.7", "coverageClass": "coverage-high", "linesTotal": "328", "linesExec": "304", "linesCoverage": "92.7", "linesClass": "coverage-high", "functionsCoverage": "96.6", "functionsClass": "coverage-high", "branchesCoverage": "66.5", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.posix_signal_service.hpp.9435b4aedce08336ba41d7447716fdc4.html", "children": []}, {"name": "posix_stream_file.hpp", "coverage": "89.0", "coverageClass": "coverage-medium", "linesTotal": "154", "linesExec": "137", "linesCoverage": "89.0", "linesClass": "coverage-medium", "functionsCoverage": "95.7", "functionsClass": "coverage-high", "branchesCoverage": "79.7", "branchesClass": "coverage-medium", "isDirectory": false, "link": "index.posix_stream_file.hpp.cea559be82a25d2e05a070397cbe4024.html", "children": []}, {"name": "posix_stream_file_service.hpp", "coverage": "89.6", "coverageClass": "coverage-medium", "linesTotal": "164", "linesExec": "147", "linesCoverage": "89.6", "linesClass": "coverage-medium", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "53.2", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.posix_stream_file_service.hpp.46b6b0daa9eaf5d66491a11742cf317c.html", "children": []}]}, {"name": "reactor", "coverage": "89.7", "coverageClass": "coverage-medium", "linesTotal": "2297", "linesExec": "2060", "linesCoverage": "89.7", "linesClass": "coverage-medium", "functionsCoverage": "94.9", "functionsClass": "coverage-high", "branchesCoverage": "39.0", "branchesClass": "coverage-low", "isDirectory": true, "link": "index.reactor.33adc1d2c1b889d4d21e5ac1e1a724c3.html", "children": [{"name": "reactor_acceptor.hpp", "coverage": "95.7", "coverageClass": "coverage-high", "linesTotal": "211", "linesExec": "202", "linesCoverage": "95.7", "linesClass": "coverage-high", "functionsCoverage": "95.2", "functionsClass": "coverage-high", "branchesCoverage": "45.6", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.reactor_acceptor.hpp.4c70fdb86384df4057dc335749c24389.html", "children": []}, {"name": "reactor_acceptor_service.hpp", "coverage": "97.8", "coverageClass": "coverage-high", "linesTotal": "45", "linesExec": "44", "linesCoverage": "97.8", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "50.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.reactor_acceptor_service.hpp.50be32e3a1347288c6786a2ddb4b7d0f.html", "children": []}, {"name": "reactor_backend.hpp", "coverage": "86.8", "coverageClass": "coverage-medium", "linesTotal": "76", "linesExec": "66", "linesCoverage": "86.8", "linesClass": "coverage-medium", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "26.6", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.reactor_backend.hpp.7dd1935fbe4332479a22aa256d913ad2.html", "children": []}, {"name": "reactor_basic_socket.hpp", "coverage": "94.3", "coverageClass": "coverage-high", "linesTotal": "175", "linesExec": "165", "linesCoverage": "94.3", "linesClass": "coverage-high", "functionsCoverage": "95.5", "functionsClass": "coverage-high", "branchesCoverage": "31.1", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.reactor_basic_socket.hpp.af90ce160aa926ac6e03143fdc23d177.html", "children": []}, {"name": "reactor_datagram_ops.hpp", "coverage": "95.0", "coverageClass": "coverage-high", "linesTotal": "20", "linesExec": "19", "linesCoverage": "95.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "50.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.reactor_datagram_ops.hpp.a67f936897f3cb4f7ba563260152c0b8.html", "children": []}, {"name": "reactor_datagram_socket.hpp", "coverage": "89.4", "coverageClass": "coverage-medium", "linesTotal": "416", "linesExec": "372", "linesCoverage": "89.4", "linesClass": "coverage-medium", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "46.6", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.reactor_datagram_socket.hpp.209844be1bcd1c644cbca1442c332d9d.html", "children": []}, {"name": "reactor_descriptor_state.hpp", "coverage": "80.8", "coverageClass": "coverage-medium", "linesTotal": "130", "linesExec": "105", "linesCoverage": "80.8", "linesClass": "coverage-medium", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "71.4", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.reactor_descriptor_state.hpp.470fda1e41a532e81dc22a1439292312.html", "children": []}, {"name": "reactor_op.hpp", "coverage": "80.5", "coverageClass": "coverage-medium", "linesTotal": "210", "linesExec": "169", "linesCoverage": "80.5", "linesClass": "coverage-medium", "functionsCoverage": "85.2", "functionsClass": "coverage-medium", "branchesCoverage": "12.9", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.reactor_op.hpp.63ce9c80a14e6696aadf3bfdbf270942.html", "children": []}, {"name": "reactor_op_base.hpp", "coverage": "90.9", "coverageClass": "coverage-high", "linesTotal": "11", "linesExec": "10", "linesCoverage": "90.9", "linesClass": "coverage-high", "functionsCoverage": "80.0", "functionsClass": "coverage-medium", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.reactor_op_base.hpp.3698636fc676388a9274f1fbeb66e527.html", "children": []}, {"name": "reactor_op_complete.hpp", "coverage": "97.5", "coverageClass": "coverage-high", "linesTotal": "122", "linesExec": "119", "linesCoverage": "97.5", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "54.9", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.reactor_op_complete.hpp.6b7ba971c7370883fdfef3875166c9c9.html", "children": []}, {"name": "reactor_scheduler.hpp", "coverage": "84.3", "coverageClass": "coverage-medium", "linesTotal": "383", "linesExec": "323", "linesCoverage": "84.3", "linesClass": "coverage-medium", "functionsCoverage": "89.8", "functionsClass": "coverage-medium", "branchesCoverage": "62.3", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.reactor_scheduler.hpp.e00583c613498920bc0adaad25307803.html", "children": []}, {"name": "reactor_service_finals.hpp", "coverage": "89.2", "coverageClass": "coverage-medium", "linesTotal": "120", "linesExec": "107", "linesCoverage": "89.2", "linesClass": "coverage-medium", "functionsCoverage": "97.3", "functionsClass": "coverage-high", "branchesCoverage": "37.9", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.reactor_service_finals.hpp.ef780764aba0613bb2672c81d89891ab.html", "children": []}, {"name": "reactor_service_state.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "1", "linesExec": "1", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.reactor_service_state.hpp.a06f4883cc3165002e377a44c715834a.html", "children": []}, {"name": "reactor_socket_finals.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "21", "linesExec": "21", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.reactor_socket_finals.hpp.49a4ceac24043e598d6baf78415bb9c8.html", "children": []}, {"name": "reactor_socket_service.hpp", "coverage": "93.5", "coverageClass": "coverage-high", "linesTotal": "46", "linesExec": "43", "linesCoverage": "93.5", "linesClass": "coverage-high", "functionsCoverage": "93.5", "functionsClass": "coverage-high", "branchesCoverage": "43.8", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.reactor_socket_service.hpp.b284eeebeaad07f482b30ea58199d0d6.html", "children": []}, {"name": "reactor_stream_ops.hpp", "coverage": "94.7", "coverageClass": "coverage-high", "linesTotal": "19", "linesExec": "18", "linesCoverage": "94.7", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "37.5", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.reactor_stream_ops.hpp.a969589094e97cb6dfc2d6fbd2dbb891.html", "children": []}, {"name": "reactor_stream_socket.hpp", "coverage": "94.8", "coverageClass": "coverage-high", "linesTotal": "291", "linesExec": "276", "linesCoverage": "94.8", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "34.7", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.reactor_stream_socket.hpp.bbb7fc268461fd7e60fdf1b01818aca9.html", "children": []}]}, {"name": "select", "coverage": "84.1", "coverageClass": "coverage-medium", "linesTotal": "315", "linesExec": "265", "linesCoverage": "84.1", "linesClass": "coverage-medium", "functionsCoverage": "98.1", "functionsClass": "coverage-high", "branchesCoverage": "45.9", "branchesClass": "coverage-low", "isDirectory": true, "link": "index.select.9dd6dbd0505ff0eebaa0b6b503a8e747.html", "children": [{"name": "select_scheduler.hpp", "coverage": "87.6", "coverageClass": "coverage-medium", "linesTotal": "178", "linesExec": "156", "linesCoverage": "87.6", "linesClass": "coverage-medium", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "51.7", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.select_scheduler.hpp.963d28eaa1e5e6f82ac56eb4f8fab6cf.html", "children": []}, {"name": "select_traits.hpp", "coverage": "74.8", "coverageClass": "coverage-low", "linesTotal": "103", "linesExec": "77", "linesCoverage": "74.8", "linesClass": "coverage-low", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "36.4", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.select_traits.hpp.ed612cecd8195990bd88a93f7c620440.html", "children": []}, {"name": "select_types.hpp", "coverage": "94.1", "coverageClass": "coverage-high", "linesTotal": "34", "linesExec": "32", "linesCoverage": "94.1", "linesClass": "coverage-high", "functionsCoverage": "96.6", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.select_types.hpp.22460019077152cc8b5046fccb7fab5a.html", "children": []}]}, {"name": "coro_op.hpp", "coverage": "94.1", "coverageClass": "coverage-high", "linesTotal": "17", "linesExec": "16", "linesCoverage": "94.1", "linesClass": "coverage-high", "functionsCoverage": "83.3", "functionsClass": "coverage-medium", "branchesCoverage": "100.0", "branchesClass": "coverage-high", "isDirectory": false, "link": "index.coro_op.hpp.2dd99468f066e3a6afa57778fcd512b0.html", "children": []}, {"name": "coro_op_complete.hpp", "coverage": "94.1", "coverageClass": "coverage-high", "linesTotal": "17", "linesExec": "16", "linesCoverage": "94.1", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "75.0", "branchesClass": "coverage-medium", "isDirectory": false, "link": "index.coro_op_complete.hpp.f87ba72e71ab3183c7395145fb330150.html", "children": []}, {"name": "endpoint_convert.hpp", "coverage": "93.5", "coverageClass": "coverage-high", "linesTotal": "108", "linesExec": "101", "linesCoverage": "93.5", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "59.1", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.endpoint_convert.hpp.4418b6b0778633ab97da1a9f24ce0f40.html", "children": []}, {"name": "make_err.hpp", "coverage": "71.4", "coverageClass": "coverage-low", "linesTotal": "7", "linesExec": "5", "linesCoverage": "71.4", "linesClass": "coverage-low", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "50.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.make_err.hpp.3fac0e81cde01ef22bfa6dc7e737aaf6.html", "children": []}, {"name": "msg_flags.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "6", "linesExec": "6", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "66.7", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.msg_flags.hpp.19b05e398a1d8e278de25a1acad49980.html", "children": []}, {"name": "native_socket_base.hpp", "coverage": "97.3", "coverageClass": "coverage-high", "linesTotal": "37", "linesExec": "36", "linesCoverage": "97.3", "linesClass": "coverage-high", "functionsCoverage": "93.1", "functionsClass": "coverage-high", "branchesCoverage": "37.2", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.native_socket_base.hpp.7209b2aa2a507ce05316bf629ef236ff.html", "children": []}]}, {"name": "native_cancel.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "6", "linesExec": "6", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.native_cancel.hpp.f20ab96a2c96fc7ee984a6436339f42b.html", "children": []}, {"name": "native_io_context.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "48", "linesExec": "48", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "92.9", "branchesClass": "coverage-high", "isDirectory": false, "link": "index.native_io_context.hpp.2902a62851a0168a12e9dd2b77d8ed76.html", "children": []}, {"name": "native_local_datagram_socket.hpp", "coverage": "92.3", "coverageClass": "coverage-high", "linesTotal": "156", "linesExec": "144", "linesCoverage": "92.3", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "48.6", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.native_local_datagram_socket.hpp.adefd487c1722c013903a22bffb95985.html", "children": []}, {"name": "native_local_stream_acceptor.hpp", "coverage": "85.9", "coverageClass": "coverage-medium", "linesTotal": "78", "linesExec": "67", "linesCoverage": "85.9", "linesClass": "coverage-medium", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "41.7", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.native_local_stream_acceptor.hpp.ac0299282dbd31209b4fabfe4c6039ff.html", "children": []}, {"name": "native_local_stream_socket.hpp", "coverage": "90.8", "coverageClass": "coverage-high", "linesTotal": "87", "linesExec": "79", "linesCoverage": "90.8", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "40.5", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.native_local_stream_socket.hpp.f347771aa0a6816d25c0a56897006d70.html", "children": []}, {"name": "native_random_access_file.hpp", "coverage": "92.0", "coverageClass": "coverage-high", "linesTotal": "50", "linesExec": "46", "linesCoverage": "92.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "40.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.native_random_access_file.hpp.6156301cfc23415fb6fcd447d2818908.html", "children": []}, {"name": "native_resolver.hpp", "coverage": "92.3", "coverageClass": "coverage-high", "linesTotal": "26", "linesExec": "24", "linesCoverage": "92.3", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "42.9", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.native_resolver.hpp.234718d9018f6559f6d3da7fd0330e43.html", "children": []}, {"name": "native_signal_set.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "7", "linesExec": "7", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.native_signal_set.hpp.9ffc603fcc79fece95e8ec3db6da594d.html", "children": []}, {"name": "native_socket_option.hpp", "coverage": "93.9", "coverageClass": "coverage-high", "linesTotal": "179", "linesExec": "168", "linesCoverage": "93.9", "linesClass": "coverage-high", "functionsCoverage": "95.6", "functionsClass": "coverage-high", "branchesCoverage": "37.5", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.native_socket_option.hpp.a76639c5921dee2dc753160de6371063.html", "children": []}, {"name": "native_stream_file.hpp", "coverage": "91.3", "coverageClass": "coverage-high", "linesTotal": "46", "linesExec": "42", "linesCoverage": "91.3", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "40.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.native_stream_file.hpp.7a9e1daf81b4df7585ebcddfea80d27f.html", "children": []}, {"name": "native_tcp.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "11", "linesExec": "11", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.native_tcp.hpp.860f8a39f5e681dd336f2ca49f6c1e23.html", "children": []}, {"name": "native_tcp_acceptor.hpp", "coverage": "88.0", "coverageClass": "coverage-medium", "linesTotal": "75", "linesExec": "66", "linesCoverage": "88.0", "linesClass": "coverage-medium", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "40.5", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.native_tcp_acceptor.hpp.ca18329a9f9124c80f59e965af1bd489.html", "children": []}, {"name": "native_tcp_socket.hpp", "coverage": "89.8", "coverageClass": "coverage-medium", "linesTotal": "88", "linesExec": "79", "linesCoverage": "89.8", "linesClass": "coverage-medium", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "40.5", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.native_tcp_socket.hpp.5b2ee26d3e3b7fdb23392228d56d892d.html", "children": []}, {"name": "native_timer.hpp", "coverage": "94.9", "coverageClass": "coverage-high", "linesTotal": "39", "linesExec": "37", "linesCoverage": "94.9", "linesClass": "coverage-high", "functionsCoverage": "92.9", "functionsClass": "coverage-high", "branchesCoverage": "50.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.native_timer.hpp.cc7c6585a900956aa4880723978b9f96.html", "children": []}, {"name": "native_udp.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "11", "linesExec": "11", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.native_udp.hpp.dce8a71d745916ea7cb962eda2e1430c.html", "children": []}, {"name": "native_udp_socket.hpp", "coverage": "89.8", "coverageClass": "coverage-medium", "linesTotal": "157", "linesExec": "141", "linesCoverage": "89.8", "linesClass": "coverage-medium", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "43.1", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.native_udp_socket.hpp.27547cdde3569fb4f3e3816d76a082bf.html", "children": []}]}, {"name": "cancel.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "12", "linesExec": "12", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.cancel.hpp.4b7ab7a7142bc98ab224ef8189c6d75b.html", "children": []}, {"name": "connect.hpp", "coverage": "90.2", "coverageClass": "coverage-high", "linesTotal": "51", "linesExec": "46", "linesCoverage": "90.2", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "42.6", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.connect.hpp.4b18fdcd064b35e8c4b9afcf705cb8f0.html", "children": []}, {"name": "endpoint.hpp", "coverage": "98.7", "coverageClass": "coverage-high", "linesTotal": "75", "linesExec": "74", "linesCoverage": "98.7", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "91.7", "branchesClass": "coverage-high", "isDirectory": false, "link": "index.endpoint.hpp.1ccb342d895a11aee228d921682dd0b4.html", "children": []}, {"name": "file_base.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "6", "linesExec": "6", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.file_base.hpp.a5ed66ad8445c6022f695288d06839a2.html", "children": []}, {"name": "io_context.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "100", "linesExec": "100", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "78.6", "branchesClass": "coverage-medium", "isDirectory": false, "link": "index.io_context.hpp.2cea8a014e7c11575d09a9def3b8cf05.html", "children": []}, {"name": "ipv4_address.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "12", "linesExec": "12", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.ipv4_address.hpp.1f1cc02a319c00d14efc1b9bbac380a9.html", "children": []}, {"name": "ipv6_address.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "10", "linesExec": "10", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "50.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.ipv6_address.hpp.e700c8b05e8de3160034cb406c3490eb.html", "children": []}, {"name": "local_datagram_socket.hpp", "coverage": "90.5", "coverageClass": "coverage-high", "linesTotal": "95", "linesExec": "86", "linesCoverage": "90.5", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "60.4", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.local_datagram_socket.hpp.72d39ca44020a0294da48e508b04dfb8.html", "children": []}, {"name": "local_endpoint.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "18", "linesExec": "18", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "75.0", "branchesClass": "coverage-medium", "isDirectory": false, "link": "index.local_endpoint.hpp.3e32cf6ac5a5b063f9a1ec9c09a29d89.html", "children": []}, {"name": "local_stream_acceptor.hpp", "coverage": "92.1", "coverageClass": "coverage-high", "linesTotal": "63", "linesExec": "58", "linesCoverage": "92.1", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "52.4", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.local_stream_acceptor.hpp.fd9d9f6fda6741a3c9ae7a53ffd1e262.html", "children": []}, {"name": "local_stream_socket.hpp", "coverage": "87.8", "coverageClass": "coverage-medium", "linesTotal": "49", "linesExec": "43", "linesCoverage": "87.8", "linesClass": "coverage-medium", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "60.7", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.local_stream_socket.hpp.30936fa7bd58c6f9105f5fed699402b9.html", "children": []}, {"name": "openssl_stream.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "9", "linesExec": "9", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "50.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.openssl_stream.hpp.a681b43f6d7c79d10fb15a94a8325fee.html", "children": []}, {"name": "random_access_file.hpp", "coverage": "92.2", "coverageClass": "coverage-high", "linesTotal": "51", "linesExec": "47", "linesCoverage": "92.2", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "55.6", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.random_access_file.hpp.3df17c93a825f0e3f5dbe0f5580b43c1.html", "children": []}, {"name": "resolver.hpp", "coverage": "97.5", "coverageClass": "coverage-high", "linesTotal": "79", "linesExec": "77", "linesCoverage": "97.5", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "57.7", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.resolver.hpp.36c23bee17a1fb182ed9a4814af15d5c.html", "children": []}, {"name": "resolver_results.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "25", "linesExec": "25", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "50.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.resolver_results.hpp.e0e3929165fcba0051433bd348171a70.html", "children": []}, {"name": "signal_set.hpp", "coverage": "96.4", "coverageClass": "coverage-high", "linesTotal": "28", "linesExec": "27", "linesCoverage": "96.4", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "41.7", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.signal_set.hpp.b464843277cf8d1a57c243385cacb103.html", "children": []}, {"name": "socket_option.hpp", "coverage": "94.0", "coverageClass": "coverage-high", "linesTotal": "84", "linesExec": "79", "linesCoverage": "94.0", "linesClass": "coverage-high", "functionsCoverage": "86.2", "functionsClass": "coverage-medium", "branchesCoverage": "50.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.socket_option.hpp.64f94fe7e541488f95ee3c316d639895.html", "children": []}, {"name": "stream_file.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "14", "linesExec": "14", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "66.7", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.stream_file.hpp.8a55105cfad04621267e0aec1894ae5e.html", "children": []}, {"name": "tcp.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "7", "linesExec": "7", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.tcp.hpp.0df59b6d5a41776b7d45a5ee1f5a0ad8.html", "children": []}, {"name": "tcp_acceptor.hpp", "coverage": "90.2", "coverageClass": "coverage-high", "linesTotal": "92", "linesExec": "83", "linesCoverage": "90.2", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "56.5", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.tcp_acceptor.hpp.66aa8d0eaf61300e4447202694510faf.html", "children": []}, {"name": "tcp_server.hpp", "coverage": "93.9", "coverageClass": "coverage-high", "linesTotal": "164", "linesExec": "154", "linesCoverage": "93.9", "linesClass": "coverage-high", "functionsCoverage": "98.0", "functionsClass": "coverage-high", "branchesCoverage": "56.9", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.tcp_server.hpp.fff21b793b86c6deaeace2bd6cead48c.html", "children": []}, {"name": "tcp_socket.hpp", "coverage": "91.3", "coverageClass": "coverage-high", "linesTotal": "46", "linesExec": "42", "linesCoverage": "91.3", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "51.6", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.tcp_socket.hpp.1535b36c2cf90e33da4787e6f2956c13.html", "children": []}, {"name": "timer.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "38", "linesExec": "38", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "78.6", "branchesClass": "coverage-medium", "isDirectory": false, "link": "index.timer.hpp.5287b1d0857c486135d08347ad081eb9.html", "children": []}, {"name": "tls_context.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "11", "linesExec": "11", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "50.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.tls_context.hpp.1d4823b6169e9f72026e1ed24ea99d14.html", "children": []}, {"name": "tls_stream.hpp", "coverage": "75.0", "coverageClass": "coverage-medium", "linesTotal": "8", "linesExec": "6", "linesCoverage": "75.0", "linesClass": "coverage-medium", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "50.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.tls_stream.hpp.7b6ddf0febe61fa5e4aa9fd1fc6385e0.html", "children": []}, {"name": "udp.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "5", "linesExec": "5", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.udp.hpp.8de0d5efc14117d24453e8e0ea1ab7f4.html", "children": []}, {"name": "udp_socket.hpp", "coverage": "93.0", "coverageClass": "coverage-high", "linesTotal": "100", "linesExec": "93", "linesCoverage": "93.0", "linesClass": "coverage-high", "functionsCoverage": "94.0", "functionsClass": "coverage-high", "branchesCoverage": "55.6", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.udp_socket.hpp.2b59f8c82adb67a9a737a891653419f8.html", "children": []}]}]}]}, {"name": "src", "coverage": "92.3", "coverageClass": "coverage-high", "linesTotal": "2090", "linesExec": "1929", "linesCoverage": "92.3", "linesClass": "coverage-high", "functionsCoverage": "98.8", "functionsClass": "coverage-high", "branchesCoverage": "55.7", "branchesClass": "coverage-low", "isDirectory": true, "link": "index.src.25d902c24283ab8cfbac54dfa101ad31.html", "children": [{"name": "corosio", "coverage": "95.3", "coverageClass": "coverage-high", "linesTotal": "1617", "linesExec": "1541", "linesCoverage": "95.3", "linesClass": "coverage-high", "functionsCoverage": "98.6", "functionsClass": "coverage-high", "branchesCoverage": "69.4", "branchesClass": "coverage-low", "isDirectory": true, "link": "index.corosio.138ea91f07444560d091012a4c0bf413.html", "children": [{"name": "src", "coverage": "", "coverageClass": "coverage-unknown", "linesTotal": "", "linesExec": "", "linesCoverage": "", "linesClass": "", "functionsCoverage": "", "functionsClass": "", "branchesCoverage": "", "branchesClass": "", "isDirectory": true, "link": null, "children": [{"name": "detail", "coverage": "", "coverageClass": "coverage-unknown", "linesTotal": "", "linesExec": "", "linesCoverage": "", "linesClass": "", "functionsCoverage": "", "functionsClass": "", "branchesCoverage": "", "branchesClass": "", "isDirectory": true, "link": null, "children": [{"name": "except.cpp", "coverage": "66.7", "coverageClass": "coverage-low", "linesTotal": "12", "linesExec": "8", "linesCoverage": "66.7", "linesClass": "coverage-low", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "50.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.except.cpp.77a1476dac0550a2caab85549a41315b.html", "children": []}]}, {"name": "tls", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "147", "linesExec": "147", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "59.3", "branchesClass": "coverage-low", "isDirectory": true, "link": "index.tls.7a4bd820adff7e41e3194f5686531851.html", "children": [{"name": "detail", "coverage": "", "coverageClass": "coverage-unknown", "linesTotal": "", "linesExec": "", "linesCoverage": "", "linesClass": "", "functionsCoverage": "", "functionsClass": "", "branchesCoverage": "", "branchesClass": "", "isDirectory": true, "link": null, "children": [{"name": "context_impl.hpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "34", "linesExec": "34", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "70.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.context_impl.hpp.2858910a506ed277b6b9e302946818fa.html", "children": []}]}, {"name": "context.cpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "113", "linesExec": "113", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "57.9", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.context.cpp.31fb1279dd54bff360808a88d3d0ae7b.html", "children": []}]}, {"name": "endpoint.cpp", "coverage": "92.6", "coverageClass": "coverage-high", "linesTotal": "81", "linesExec": "75", "linesCoverage": "92.6", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "80.0", "branchesClass": "coverage-medium", "isDirectory": false, "link": "index.endpoint.cpp.cea18beb4a7708484ea5a7a87894850a.html", "children": []}, {"name": "host_name.cpp", "coverage": "44.4", "coverageClass": "coverage-low", "linesTotal": "9", "linesExec": "4", "linesCoverage": "44.4", "linesClass": "coverage-low", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "11.1", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.host_name.cpp.0d80e42f635aac358a1138f7384d39b6.html", "children": []}, {"name": "io_context.cpp", "coverage": "98.9", "coverageClass": "coverage-high", "linesTotal": "95", "linesExec": "94", "linesCoverage": "98.9", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "69.4", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.io_context.cpp.307c2f8fee259a7dded1659580a4cec6.html", "children": []}, {"name": "ipv4_address.cpp", "coverage": "98.2", "coverageClass": "coverage-high", "linesTotal": "109", "linesExec": "107", "linesCoverage": "98.2", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "80.3", "branchesClass": "coverage-medium", "isDirectory": false, "link": "index.ipv4_address.cpp.6bdd2c749cd246cd38b3b0eb3d410abc.html", "children": []}, {"name": "ipv6_address.cpp", "coverage": "97.0", "coverageClass": "coverage-high", "linesTotal": "266", "linesExec": "258", "linesCoverage": "97.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "85.3", "branchesClass": "coverage-medium", "isDirectory": false, "link": "index.ipv6_address.cpp.5456b7100ae193e1f7bb54707b36bc99.html", "children": []}, {"name": "local_connect_pair.cpp", "coverage": "67.4", "coverageClass": "coverage-low", "linesTotal": "46", "linesExec": "31", "linesCoverage": "67.4", "linesClass": "coverage-low", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "34.5", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.local_connect_pair.cpp.7328ccdb02621baacf87e122b82ce7c0.html", "children": []}, {"name": "local_datagram.cpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "6", "linesExec": "6", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.local_datagram.cpp.3ab3eaee4dab2a13c78a0e682afec0f1.html", "children": []}, {"name": "local_datagram_socket.cpp", "coverage": "95.2", "coverageClass": "coverage-high", "linesTotal": "84", "linesExec": "80", "linesCoverage": "95.2", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "87.5", "branchesClass": "coverage-medium", "isDirectory": false, "link": "index.local_datagram_socket.cpp.bfd64e9c528b999e3f214d3633582ce7.html", "children": []}, {"name": "local_endpoint.cpp", "coverage": "85.7", "coverageClass": "coverage-medium", "linesTotal": "28", "linesExec": "24", "linesCoverage": "85.7", "linesClass": "coverage-medium", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "87.5", "branchesClass": "coverage-medium", "isDirectory": false, "link": "index.local_endpoint.cpp.b56d93a22274b47be6ebb99942813e0a.html", "children": []}, {"name": "local_stream.cpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "6", "linesExec": "6", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.local_stream.cpp.4f61b1b880068fd9590883a8bb8efc28.html", "children": []}, {"name": "local_stream_acceptor.cpp", "coverage": "96.8", "coverageClass": "coverage-high", "linesTotal": "62", "linesExec": "60", "linesCoverage": "96.8", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "79.2", "branchesClass": "coverage-medium", "isDirectory": false, "link": "index.local_stream_acceptor.cpp.a055710320d1b6565922b720c227c1e1.html", "children": []}, {"name": "local_stream_socket.cpp", "coverage": "94.8", "coverageClass": "coverage-high", "linesTotal": "77", "linesExec": "73", "linesCoverage": "94.8", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "86.7", "branchesClass": "coverage-medium", "isDirectory": false, "link": "index.local_stream_socket.cpp.0038a8e5d6a4ed2a9a559a28308163e4.html", "children": []}, {"name": "random_access_file.cpp", "coverage": "98.4", "coverageClass": "coverage-high", "linesTotal": "64", "linesExec": "63", "linesCoverage": "98.4", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "91.7", "branchesClass": "coverage-high", "isDirectory": false, "link": "index.random_access_file.cpp.d8164d9f3b098b50611eb93c737d7c25.html", "children": []}, {"name": "resolver.cpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "9", "linesExec": "9", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "50.0", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.resolver.cpp.33a7fb069201cf9836b5ff5ff5ca4f5a.html", "children": []}, {"name": "signal_set.cpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "25", "linesExec": "25", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "66.7", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.signal_set.cpp.c4a13883c8d71b90dd9eded9f634242c.html", "children": []}, {"name": "socket_option.cpp", "coverage": "92.0", "coverageClass": "coverage-high", "linesTotal": "138", "linesExec": "127", "linesCoverage": "92.0", "linesClass": "coverage-high", "functionsCoverage": "92.6", "functionsClass": "coverage-high", "branchesCoverage": "-", "branchesClass": "coverage-unknown", "isDirectory": false, "link": "index.socket_option.cpp.17b4922b361e28ebde4956f9f4707b9b.html", "children": []}, {"name": "stream_file.cpp", "coverage": "98.6", "coverageClass": "coverage-high", "linesTotal": "69", "linesExec": "68", "linesCoverage": "98.6", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "92.3", "branchesClass": "coverage-high", "isDirectory": false, "link": "index.stream_file.cpp.b9ae75873a844b6cc2204d2b931dc281.html", "children": []}, {"name": "tcp.cpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "6", "linesExec": "6", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "100.0", "branchesClass": "coverage-high", "isDirectory": false, "link": "index.tcp.cpp.4b8b3b60a18b22b2bf89c77b9f9d3d72.html", "children": []}, {"name": "tcp_acceptor.cpp", "coverage": "96.3", "coverageClass": "coverage-high", "linesTotal": "54", "linesExec": "52", "linesCoverage": "96.3", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "46.2", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.tcp_acceptor.cpp.43d36599e2a378101559cfcb4948b753.html", "children": []}, {"name": "tcp_server.cpp", "coverage": "95.6", "coverageClass": "coverage-high", "linesTotal": "90", "linesExec": "86", "linesCoverage": "95.6", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "56.8", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.tcp_server.cpp.ce89588d51f0455c50ba8264c578e5c0.html", "children": []}, {"name": "tcp_socket.cpp", "coverage": "98.2", "coverageClass": "coverage-high", "linesTotal": "57", "linesExec": "56", "linesCoverage": "98.2", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "90.0", "branchesClass": "coverage-high", "isDirectory": false, "link": "index.tcp_socket.cpp.d57ea6bceb5cfd285dfc8522fa1cd86e.html", "children": []}, {"name": "timer.cpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "19", "linesExec": "19", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "33.3", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.timer.cpp.0f02a151f6c13471bf5fdcf3706ae581.html", "children": []}, {"name": "udp.cpp", "coverage": "100.0", "coverageClass": "coverage-high", "linesTotal": "6", "linesExec": "6", "linesCoverage": "100.0", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "100.0", "branchesClass": "coverage-high", "isDirectory": false, "link": "index.udp.cpp.eeb976458a2bf2d7bff3c04de9948280.html", "children": []}, {"name": "udp_socket.cpp", "coverage": "98.1", "coverageClass": "coverage-high", "linesTotal": "52", "linesExec": "51", "linesCoverage": "98.1", "linesClass": "coverage-high", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "88.9", "branchesClass": "coverage-medium", "isDirectory": false, "link": "index.udp_socket.cpp.d574386ae9422ff336a82e4db65a3f2b.html", "children": []}]}]}, {"name": "openssl", "coverage": "", "coverageClass": "coverage-unknown", "linesTotal": "", "linesExec": "", "linesCoverage": "", "linesClass": "", "functionsCoverage": "", "functionsClass": "", "branchesCoverage": "", "branchesClass": "", "isDirectory": true, "link": null, "children": [{"name": "src", "coverage": "", "coverageClass": "coverage-unknown", "linesTotal": "", "linesExec": "", "linesCoverage": "", "linesClass": "", "functionsCoverage": "", "functionsClass": "", "branchesCoverage": "", "branchesClass": "", "isDirectory": true, "link": null, "children": [{"name": "openssl_stream.cpp", "coverage": "82.0", "coverageClass": "coverage-medium", "linesTotal": "473", "linesExec": "388", "linesCoverage": "82.0", "linesClass": "coverage-medium", "functionsCoverage": "100.0", "functionsClass": "coverage-high", "branchesCoverage": "44.5", "branchesClass": "coverage-low", "isDirectory": false, "link": "index.openssl_stream.cpp.6da1fcbbf43740376788c7eac26aae4b.html", "children": []}]}]}]}];
