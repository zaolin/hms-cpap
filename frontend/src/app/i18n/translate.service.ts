import { Injectable } from '@angular/core';
import { de } from './de';
import { en } from './en';

type Lang = 'de' | 'en';

@Injectable({ providedIn: 'root' })
export class TranslateService {
  private lang: Lang = 'en';
  private dicts: Record<Lang, Record<string, string>> = { de, en };

  setLang(lang: string) {
    if (lang === 'de' || lang === 'en') {
      this.lang = lang as Lang;
    }
  }

  get currentLang(): string {
    return this.lang;
  }

  get(key: string): string {
    return this.dicts[this.lang][key] ?? this.dicts.en[key] ?? key;
  }
}