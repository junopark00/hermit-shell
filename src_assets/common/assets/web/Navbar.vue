<template>
  <nav class="navbar navbar-expand-lg header shell-header">
    <div class="container-fluid">
      <a class="navbar-brand shell-brand" href="./" title="Shell">
        <img src="/images/logo-shell-45.png" width="24" height="24" alt="" style="image-rendering: pixelated">
        <span class="shell-wordmark">Shell</span>
      </a>
      <button class="navbar-toggler" type="button" data-bs-toggle="collapse" data-bs-target="#navbarSupportedContent"
              aria-controls="navbarSupportedContent" aria-expanded="false" :aria-label="$t('navbar.toggle_menu')">
        <span class="navbar-toggler-icon"></span>
      </button>
      <div class="collapse navbar-collapse" id="navbarSupportedContent">
        <ul class="navbar-nav me-auto">
          <li class="nav-item">
            <a class="nav-link" href="./"><i class="fas fa-fw fa-house"></i> {{ $t('navbar.home') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./pin"><i class="fas fa-fw fa-link"></i> {{ $t('navbar.pin') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./apps"><i class="fas fa-fw fa-table-list"></i> {{ $t('navbar.applications') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./config"><i class="fas fa-fw fa-sliders"></i> {{ $t('navbar.configuration') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./password"><i class="fas fa-fw fa-key"></i> {{ $t('navbar.password') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./troubleshooting"><i class="fas fa-fw fa-screwdriver-wrench"></i> {{ $t('navbar.troubleshoot') }}</a>
          </li>
        </ul>
        <ul class="navbar-nav">
          <li class="nav-item">
            <ThemeToggle/>
          </li>
        </ul>
      </div>
    </div>
  </nav>
</template>

<script>
import ThemeToggle from './ThemeToggle.vue'

export default {
  components: { ThemeToggle },
  created() {
    console.log("Header mounted!")
  },
  mounted() {
    // Links are relative ("./config"), so compare resolved paths rather than the href text.
    const trim = p => p.replace(/\/$/, '') || '/';
    const here = trim(document.location.pathname);
    for (const el of this.$el.querySelectorAll("a.nav-link[href]")) {
      if (trim(new URL(el.href).pathname) === here) el.classList.add("active");
    }
  }
}
</script>

<style>
/* Shell header: a 48px dark bar in both themes, teal bottom border on the active item. */
.shell-header {
  --bs-navbar-padding-y: 0;
  --bs-navbar-padding-x: 0;
  --bs-navbar-nav-link-padding-x: 16px;
  background-color: var(--shell-header-bg, #161616);
  border-bottom: 1px solid var(--shell-header-border, #393939);
  min-height: 48px;
}

.shell-header .container-fluid {
  padding-left: 16px;
  padding-right: 16px;
  min-height: 48px;
}

.shell-header .shell-brand {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 0;
  margin-right: 24px;
  color: #F4F4F4;
}

.shell-header .shell-brand img {
  width: 24px;
  height: 24px;
}

.shell-header .shell-wordmark {
  font-size: 14px;
  line-height: 20px;
  font-weight: 600;
  letter-spacing: .16px;
}

.header .nav-link {
  display: flex;
  align-items: center;
  gap: 8px;
  height: 48px;
  padding: 0 16px !important;
  color: var(--shell-header-text, #C6C6C6) !important;
  font-size: 14px;
  box-shadow: inset 0 -3px 0 transparent;
}

.header .nav-link i {
  font-size: 14px;
  width: 16px;
  color: var(--shell-header-text, #C6C6C6);
}

.header .nav-link:hover,
.header .nav-link.show {
  color: var(--shell-header-text-active, #F4F4F4) !important;
  background-color: var(--shell-header-hover, #2C2C2C);
}

.header .nav-link.active {
  color: var(--shell-header-text-active, #F4F4F4) !important;
  font-weight: 600;
  box-shadow: inset 0 -3px 0 var(--shell-header-indicator, #08BDBA);
}

.header .nav-link.active i {
  color: var(--shell-header-text-active, #F4F4F4);
}

.header .nav-link:focus-visible {
  outline: 2px solid #F4F4F4;
  outline-offset: -2px;
}

.header .navbar-toggler {
  border: 0 !important;
  border-radius: 0;
  padding: 12px;
  margin-right: -16px;
}

.header .navbar-toggler:focus {
  box-shadow: inset 0 0 0 2px #F4F4F4;
}

.header .navbar-toggler-icon {
  width: 20px;
  height: 20px;
  --bs-navbar-toggler-icon-bg: url("data:image/svg+xml,%3csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 30 30'%3e%3cpath stroke='%23F4F4F4' stroke-linecap='square' stroke-width='2' d='M4 8h22M4 15h22M4 22h22'/%3e%3c/svg%3e") !important;
}

@media (max-width: 991.98px) {
  .shell-header .navbar-collapse {
    margin: 0 -16px;
    border-top: 1px solid var(--shell-header-border, #393939);
  }

  .header .nav-link {
    height: 40px;
    box-shadow: inset 3px 0 0 transparent;
  }

  .header .nav-link.active {
    box-shadow: inset 3px 0 0 var(--shell-header-indicator, #08BDBA);
  }
}

.form-control::placeholder {
  opacity: 0.5;
}
</style>
