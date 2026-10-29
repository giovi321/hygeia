import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';

export default defineConfig({
  site: 'https://giovi321.github.io',
  base: '/hygeia',
  integrations: [
    starlight({
      title: 'Hygeia',
      description: 'Medicine reminder and dose logger for the Waveshare ESP32-S3-Touch-AMOLED-2.16 - documentation',
      customCss: ['./src/styles/theme.css'],
      logo: {
        src: './src/assets/logo.svg',
        replacesTitle: false,
      },
      social: [
        {
          icon: 'github',
          label: 'GitHub',
          href: 'https://github.com/giovi321/hygeia',
        },
      ],
      editLink: {
        baseUrl: 'https://github.com/giovi321/hygeia/edit/main/docs/',
      },
      sidebar: [
        { label: 'Home', link: '/' },
        {
          label: 'Getting started',
          items: [
            { label: 'Hardware', link: '/getting-started/hardware/' },
            { label: 'Build and flash', link: '/getting-started/flashing/' },
            { label: 'First boot', link: '/getting-started/first-boot/' },
          ],
        },
        {
          label: 'User guide',
          items: [
            { label: 'Home and logging', link: '/user-guide/home-and-logging/' },
            { label: 'Due card and reminders', link: '/user-guide/due-card/' },
            { label: 'Overview and history', link: '/user-guide/overview-history/' },
            { label: 'Medicine detail', link: '/user-guide/detail/' },
            { label: 'Quick settings and settings', link: '/user-guide/settings/' },
            { label: 'Idle clock and power', link: '/user-guide/idle-power/' },
            { label: 'Gestures', link: '/user-guide/gestures/' },
          ],
        },
        {
          label: 'Reference',
          items: [
            { label: 'Data model', link: '/reference/data-model/' },
            { label: 'Scheduling rules', link: '/reference/scheduling/' },
            { label: 'Development', link: '/reference/development/' },
          ],
        },
        { label: 'Roadmap', link: '/roadmap/' },
        { label: 'Licence', link: '/licence/' },
      ],
    }),
  ],
});
