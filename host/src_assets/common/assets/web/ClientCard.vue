<template>
    <div class="card p-2">
        <div class="card-body">
            <h2>{{ $t('client_card.clients') }}</h2>
            <br>
            <p>{{ $t('client_card.clients_desc') }}</p>
            <div class="card-group p-4 align-items-center">
                <a v-for="{ platform, icon, name, link } of clients" class="btn m-1" :class="[link && 'btn-success' || 'btn-secondary']" :href="link" target="_blank" @click="shouldOpen($event, link)"><i v-if="icon" :class="icon"></i> <span class="platform-text">{{ platform }}</span> [ {{ name }} ] </a>
            </div>
            <i>* {{ $t('client_card.generic_moonlight_clients_desc') }}</i>
        </div>
    </div>
</template>

<style scoped>
    .platform-text {
        font-weight: bold;
    }
</style>

<script setup>
    const clients = [
        {
            platform: 'Android',
            icon: 'fa-brands fa-android',
            name: 'NyxPlay',
            link: 'https://github.com/siberanka/NyxPlay/tree/main/android'
        },
        {
            platform: 'Windows / Linux / macOS',
            icon: 'fa-solid fa-desktop',
            name: 'NyxPlay',
            link: 'https://github.com/siberanka/NyxPlay/tree/main/pc'
        },
        {
            platform: 'Android',
            icon: 'fa-brands fa-android',
            name: 'Artemis',
            link: 'https://github.com/ClassicOldSong/moonlight-android'
        }
    ]

    const shouldOpen = (e, link) => {
        if (!link) {
            e.preventDefault();
        }
    }
</script>
