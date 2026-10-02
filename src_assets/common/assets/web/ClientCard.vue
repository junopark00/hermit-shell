<template>
    <div class="card p-2">
        <div class="card-body">
            <h2>{{ $t('client_card.clients') }}</h2>
            <br>
            <p>{{ $t('client_card.clients_desc') }}</p>
            <div class="card-group p-4 align-items-center">
                <a v-for="{ platform, name, link } of clients" class="btn btn-success m-1" :href="link" target="_blank"><span class="platform-text">{{ platform }}</span> · {{ name }}<i class="fas fa-fw fa-arrow-up-right-from-square ms-1"></i></a>
            </div>
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
            platform: 'Windows',
            name: 'Hermit',
            link: 'https://github.com/junopark00/hermit/releases'
        },
        {
            platform: 'Android',
            name: 'Hermit',
            link: 'https://github.com/junopark00/hermit-android/releases'
        }
    ]
</script>
