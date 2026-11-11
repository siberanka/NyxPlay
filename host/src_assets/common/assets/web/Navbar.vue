<template>
  <nav class="navbar navbar-dark navbar-expand-lg navbar-background header">
    <div class="container">
      <a class="navbar-brand d-flex align-items-center gap-2" href="./" title="NyxPlay Host">
        <img src="/images/logo-nyxplay-host-45.png" height="45" width="45" alt="NyxPlay Host">
        <span class="brand-text">NyxPlay <span class="brand-accent">Host</span></span>
      </a>
      <button class="navbar-toggler" type="button" data-bs-toggle="collapse" data-bs-target="#navbarSupportedContent"
              aria-controls="navbarSupportedContent" aria-expanded="false" aria-label="Toggle navigation">
        <span class="navbar-toggler-icon"></span>
      </button>
      <div class="collapse navbar-collapse" id="navbarSupportedContent">
        <ul class="navbar-nav me-auto mb-2 mb-lg-0">
          <li class="nav-item">
            <a class="nav-link" href="./"><i class="fas fa-fw fa-home"></i> {{ $t('navbar.home') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./pin"><i class="fas fa-fw fa-unlock"></i> {{ $t('navbar.pin') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./apps"><i class="fas fa-fw fa-stream"></i> {{ $t('navbar.applications') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./config"><i class="fas fa-fw fa-cog"></i> {{ $t('navbar.configuration') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./password"><i class="fas fa-fw fa-user-shield"></i> {{ $t('navbar.password') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./troubleshooting"><i class="fas fa-fw fa-info"></i> {{ $t('navbar.troubleshoot') }}</a>
          </li>
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
    let el = document.querySelector("a[href='" + document.location.pathname + "']");
    if (el) el.classList.add("active")
  }
}
</script>

<style>
.navbar-background {
  background: linear-gradient(100deg, var(--nyx-night) 0%, #17133f 55%, #2a1c6b 100%);
  box-shadow: 0 2px 18px rgba(11, 12, 33, .35);
}

.header .brand-text {
  color: #f4f0ff;
  font-weight: 600;
  letter-spacing: .02em;
}

.header .brand-accent {
  color: var(--nyx-lavender);
  font-weight: 400;
}

.header .nav-link {
  color: rgba(237, 233, 254, .72) !important;
  border-radius: .5rem;
}

.header .nav-link.active {
  color: #ffffff !important;
  font-weight: 500;
  background: rgba(196, 181, 253, .14);
}

.header .nav-link:hover {
  color: #ffffff !important;
  background: rgba(196, 181, 253, .10);
}

.header .navbar-toggler {
  color: rgba(237, 233, 254, .8) !important;
  border: var(--bs-border-width) solid rgba(237, 233, 254, .25) !important;
}

.header .navbar-toggler-icon {
  --bs-navbar-toggler-icon-bg: url("data:image/svg+xml,%3csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 30 30'%3e%3cpath stroke='rgba%28237, 233, 254, 0.85%29' stroke-linecap='round' stroke-miterlimit='10' stroke-width='2' d='M4 7h22M4 15h22M4 23h22'/%3e%3c/svg%3e") !important;
}

.form-control::placeholder {
  opacity: 0.5;
}
</style>
